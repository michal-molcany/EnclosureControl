#include "poller.h"

#include <WiFi.h>
#include <OctoPrinter.h>
#include <Preferences.h>

#include "const.h"
#include "secrets.h"

namespace
{
    secrets sec;
    OctoPrinter api(sec.getOctoprintApiKey(), OCTOPRINT_IP, OCTOPRINT_PORT);

    // ---------------------------------------------------------------------
    // OctoPrint address, as told to us by the C3.
    //
    // The C3 owns this value (it is the board with the web UI); this board only
    // follows it. It is persisted because this board can be power-cycled while
    // the C3 is off, and a reverted address would look like "OctoPrint is down"
    // until the link came back - which is exactly the failure that is hardest to
    // diagnose from the display alone.
    //
    // Separate NVS namespace from anything else on this board. Namespace names
    // are capped at 15 characters.
    // ---------------------------------------------------------------------
    const char *OCTO_NS = "octoend";
    const char *OCTO_KEY_IP = "ip";
    const char *OCTO_KEY_PORT = "port";

    Preferences octoPrefs;
    IPAddress currentHost;
    uint16_t currentPort = OCTOPRINT_PORT;
    bool endpointOverridden = false;

    // Pending change published by pollerSetEndpoint() from the link task and
    // consumed by pollTask(). Guard flag set last / cleared first, so the task
    // never reads a half-written struct.
    IPAddress pendingHost;
    uint16_t pendingPort = OCTOPRINT_PORT;
    volatile bool pending = false;

    void storeEndpoint(IPAddress ip, uint16_t port)
    {
        if (octoPrefs.begin(OCTO_NS, false))
        {
            octoPrefs.putUInt(OCTO_KEY_IP, (uint32_t)ip);
            octoPrefs.putUShort(OCTO_KEY_PORT, port);
            octoPrefs.end();
        }
    }

    void clearEndpoint()
    {
        if (octoPrefs.begin(OCTO_NS, false))
        {
            octoPrefs.clear();
            octoPrefs.end();
        }
    }

    // Reads the stored override before the poll task starts. A missing or
    // unreadable pair simply means "no override", which is the compiled-in
    // default - never a reason to refuse to boot.
    void loadEndpoint()
    {
        currentHost.fromString(OCTOPRINT_IP);
        currentPort = OCTOPRINT_PORT;
        endpointOverridden = false;
        if (octoPrefs.begin(OCTO_NS, true))
        {
            const uint32_t raw = octoPrefs.getUInt(OCTO_KEY_IP, 0);
            const uint16_t port = octoPrefs.getUShort(OCTO_KEY_PORT, 0);
            octoPrefs.end();
            if (raw != 0 && port != 0)
            {
                currentHost = IPAddress(raw);
                currentPort = port;
                endpointOverridden = true;
                Serial.print(F("[POLL] endpoint "));
                Serial.print(currentHost);
                Serial.print(':');
                Serial.println(currentPort);
            }
        }
    }

    // Runs on the poll task, at the top of its loop. Applies and persists in
    // one place so the stored value can never disagree with the value in force.
    void applyPendingEndpoint()
    {
        if (!pending)
            return;
        pending = false;

        const IPAddress ip = pendingHost;
        const uint16_t port = pendingPort;
        const bool isZero = ((uint32_t)ip == 0) || port == 0;

        if (isZero)
        {
            // The C3 has no override: follow it back to our compiled default, so
            // a "reset" on the C3 resets both boards.
            endpointOverridden = false;
            currentHost.fromString(OCTOPRINT_IP);
            currentPort = OCTOPRINT_PORT;
            clearEndpoint();
            api.setEndpoint(currentHost, currentPort);
            Serial.println(F("[POLL] endpoint override cleared by C3"));
            return;
        }

        if (ip == currentHost && port == currentPort && endpointOverridden)
            return;

        endpointOverridden = true;
        currentHost = ip;
        currentPort = port;
        storeEndpoint(ip, port);
        api.setEndpoint(ip, port);
        Serial.print(F("[POLL] endpoint from C3 "));
        Serial.print(ip);
        Serial.print(':');
        Serial.println(port);
    }
    SemaphoreHandle_t snapshotMutex = nullptr;
    QueueHandle_t commandQueue = nullptr;
    PrinterSnapshot shared;
    volatile bool hasData = false;
    // Printable-file cache. Heap-allocated once (see pollerStart): ~3.8 KB
    // static here overflows the CYD's DRAM segment (it links only ~800 bytes
    // under the limit), while one early heap block is harmless.
    struct FileListCache
    {
        PrintFile files[MAX_PRINT_FILES];
        uint8_t count = 0;
        uint32_t version = 0;
        bool busy = false;
        bool error = false;
    };
    FileListCache *sharedFiles = nullptr;
    uint32_t filesVersion = 0;
    // Staged print path. Written by the UI task just before it enqueues
    // PRINT_FILE, read by the poll task after dequeuing it, so the queue
    // itself provides the happens-after ordering. Confirm-gated (one dialog
    // per tap), so a second stage cannot overtake a queued first one.
    char stagedPrintPath[PRINT_FILE_PATH_LEN] = {};
    // Last file-fetch outcome, re-logged every 15 s while failed so the
    // reason is capturable on serial without tap timing. Poll-task-owned
    // (written + read on the same task); codes only, strings stay in flash.
    OctoPrinter::FetchDiag filesDiag = OctoPrinter::FETCH_OK;
    uint8_t filesParseCode = 0;
    uint32_t filesDiagAt = 0;

    // Marks the cache busy under the mutex (20 ms try, never blocks).
    void markFilesBusy()
    {
        if (snapshotMutex && sharedFiles &&
            xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(20)) == pdTRUE)
        {
            sharedFiles->busy = true;
            sharedFiles->error = false;
            xSemaphoreGive(snapshotMutex);
        }
    }

    void executeCommand(const PollerCommand &cmd)
    {
        switch (cmd.target)
        {
        case PollerCommand::Tool:
            // 0 is "off" and has its own call, matching the tool API.
            if (cmd.value == 0)
                api.preheatToolOff();
            else
                api.preheatTool(cmd.value);
            break;
        case PollerCommand::Bed:
            // 0 falls out of preheatBed as a plain 0 target.
            Serial.printf("[POLL] bed cmd %d -> HTTP %d\n", cmd.value,
                          api.preheatBed(cmd.value));
            break;
        case PollerCommand::Job:
            switch (cmd.value)
            {
            case PAUSE_JOB:
                api.pauseJob();
                break;
            case RESUME_JOB:
                api.resumeJob();
                break;
            case STOP_JOB:
                api.cancelJob();
                break;
            case REFRESH_FILES:
            {
                Serial.println("[POLL] files refresh start");
                filesDiagAt = millis();
                if (!sharedFiles)
                    break;
                // Transient heap, not static and not poll-task stack (20
                // entries are ~3.8 KB against an 8 KB task stack, and static
                // would overflow the DRAM segment - see above).
                OctoFile *scratch =
                    static_cast<OctoFile *>(malloc(sizeof(OctoFile) * MAX_PRINT_FILES));
                if (!scratch)
                {
                    Serial.println("[POLL] files refresh FAILED (no heap)");
                    filesDiag = OctoPrinter::FETCH_NOMEM;
                    filesDiagAt = millis();
                    if (xSemaphoreTake(snapshotMutex, portMAX_DELAY) == pdTRUE)
                    {
                        sharedFiles->error = true;
                        sharedFiles->busy = false;
                        xSemaphoreGive(snapshotMutex);
                    }
                    break;
                }
                OctoPrinter::FetchDiag diag = OctoPrinter::FETCH_OK;
                uint8_t pcode = 0;
                const int found = api.fetchFiles(scratch, MAX_PRINT_FILES, diag, pcode);
                filesDiag = diag;
                filesParseCode = pcode;
                filesDiagAt = millis();
                if (xSemaphoreTake(snapshotMutex, portMAX_DELAY) == pdTRUE)
                {
                    if (found >= 0)
                    {
                        sharedFiles->count = (uint8_t)found;
                        for (int i = 0; i < found; ++i)
                        {
                            strncpy(sharedFiles->files[i].path, scratch[i].path,
                                    PRINT_FILE_PATH_LEN - 1);
                            sharedFiles->files[i].path[PRINT_FILE_PATH_LEN - 1] = '\0';
                            strncpy(sharedFiles->files[i].name, scratch[i].name,
                                    PRINT_FILE_NAME_LEN - 1);
                            sharedFiles->files[i].name[PRINT_FILE_NAME_LEN - 1] = '\0';
                            sharedFiles->files[i].date = scratch[i].date;
                            sharedFiles->files[i].size = scratch[i].size;
                        }
                        sharedFiles->error = false;
                        sharedFiles->version = ++filesVersion;
                        Serial.printf("[POLL] files refresh ok count=%d version=%u\n",
                                      found, filesVersion);
                    }
                    else
                    {
                        // Keep the previous list, if any; the view reports the
                        // failure from the error flag.
                        sharedFiles->error = true;
                        Serial.println("[POLL] files refresh FAILED");
                    }
                    sharedFiles->busy = false;
                    xSemaphoreGive(snapshotMutex);
                }
                free(scratch);
                break;
            }
            case PRINT_FILE:
            {
                if (stagedPrintPath[0] == '\0')
                    break;
                // Second guard behind the idle-only entry: a job that started
                // elsewhere after the confirm opened must not be overridden.
                if (api.printing() || api.paused())
                {
                    Serial.println("[POLL] print REJECTED job active");
                    break;
                }
                Serial.printf("[POLL] print %s -> HTTP %d\n", stagedPrintPath,
                              api.selectAndPrint(stagedPrintPath));
                break;
            }
            default:
                break;
            }
            break;
        }
    }

    void publishSnapshot()
    {
        PrinterSnapshot copy;
        copy.state = api.Status();
        copy.printing = api.printing();
        copy.paused = api.paused();
        copy.toolActual = api.toolActual();
        copy.toolTarget = api.toolTarget();
        copy.bedActual = api.bedActual();
        copy.bedTarget = api.bedTarget();
        copy.chamberActual = api.chamberActual();
        copy.chamberTarget = api.chamberTarget();
        copy.progress = (float)api.progress();
        copy.remaining = api.remainingFormatted();
        copy.fileName = api.fileName();
        copy.filament = api.filamentName();
        copy.nozzle = api.nozzleDiameter();
        copy.layerCurrent = api.currentLayer();
        copy.layerTotal = api.totalLayers();
        copy.lastLayerDuration = api.lastLayerDuration();
        copy.lastLayerDurationSeconds = api.lastLayerDurationSeconds();
        copy.valid = true;
        if (xSemaphoreTake(snapshotMutex, portMAX_DELAY) == pdTRUE)
        {
            shared = copy;
            xSemaphoreGive(snapshotMutex);
        }
        hasData = true;
    }

    void pollTask(void *)
    {
        loadEndpoint();
        api.begin();
        for (;;)
        {
            applyPendingEndpoint();
PollerCommand cmd = {};
        while (xQueueReceive(commandQueue, &cmd, 0) == pdTRUE)
                executeCommand(cmd);
            if (WiFi.status() == WL_CONNECTED)
            {
                api.update();
                publishSnapshot();
            }
            // Re-log a failed file fetch every 15 s until the next attempt
            // clears it, so the reason is capturable on serial without tap
            // timing. Codes only; all strings live in flash.
            if (filesDiag != OctoPrinter::FETCH_OK &&
                (int32_t)(millis() - filesDiagAt) >= 15000)
            {
                filesDiagAt = millis();
                switch (filesDiag)
                {
                case OctoPrinter::FETCH_CONN:
                    Serial.println(F("[FILES] last: connect failed"));
                    break;
                case OctoPrinter::FETCH_HDRS:
                    Serial.println(F("[FILES] last: headers incomplete"));
                    break;
                case OctoPrinter::FETCH_PARSE:
                    Serial.printf("[FILES] last: parse code=%u heap=%u maxblock=%u\n",
                                  filesParseCode, ESP.getFreeHeap(),
                                  ESP.getMaxAllocHeap());
                    break;
                case OctoPrinter::FETCH_NOFILES:
                    Serial.println(F("[FILES] last: no files[] in response"));
                    break;
                case OctoPrinter::FETCH_NOMEM:
                    Serial.println(F("[FILES] last: no heap for scratch"));
                    break;
                default:
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(API_REFRESH_TIME));
        }
    }
} // namespace

void pollerStart()
{
    snapshotMutex = xSemaphoreCreateMutex();
    commandQueue = xQueueCreate(8, sizeof(PollerCommand));
    sharedFiles = static_cast<FileListCache *>(calloc(1, sizeof(FileListCache)));
    if (!sharedFiles)
        Serial.println("[POLL] file cache alloc FAILED; files view will report errors");
    xTaskCreatePinnedToCore(pollTask, "octoPoll", 8192, nullptr, 1, nullptr, 0);
}

bool pollerCopySnapshot(PrinterSnapshot &out)
{
    if (!snapshotMutex)
        return false;
    if (xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return false;
    out = shared;
    xSemaphoreGive(snapshotMutex);
    return true;
}

bool pollerHasData()
{
    return hasData;
}

void pollerSetEndpoint(IPAddress host, uint16_t port)
{
    // Value first, flag last: pollTask() clears the flag before reading, so a
    // change arriving mid-apply is picked up on the next pass rather than lost.
    pendingHost = host;
    pendingPort = port;
    pending = true;
}

bool pollerEndpointOverridden()
{
    return endpointOverridden;
}

void pollerSendCommand(const PollerCommand &cmd)
{
    if (commandQueue)
        xQueueSend(commandQueue, &cmd, 0);
}

bool pollerFilesMeta(uint8_t &count, uint32_t &version, bool &busy, bool &error)
{
    if (!snapshotMutex || !sharedFiles)
        return false;
    if (xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return false;
    count = sharedFiles->count;
    version = sharedFiles->version;
    busy = sharedFiles->busy;
    error = sharedFiles->error;
    xSemaphoreGive(snapshotMutex);
    return true;
}

bool pollerCopyFile(int index, PrintFile &out)
{
    if (!snapshotMutex || !sharedFiles || index < 0)
        return false;
    if (xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return false;
    const bool ok = index < sharedFiles->count;
    if (ok)
        out = sharedFiles->files[index];
    xSemaphoreGive(snapshotMutex);
    return ok;
}

void pollerRequestFilesRefresh()
{
    // Optimistic busy so the view shows "Loading..." at once instead of
    // waiting for the next poll cycle to pick up the queued command.
    markFilesBusy();
    pollerSendCommand({PollerCommand::Job, REFRESH_FILES});
}

void pollerRequestFilePrint(const char *path)
{
    if (!path || !*path)
        return;
    strncpy(stagedPrintPath, path, PRINT_FILE_PATH_LEN - 1);
    stagedPrintPath[PRINT_FILE_PATH_LEN - 1] = '\0';
    pollerSendCommand({PollerCommand::Job, PRINT_FILE});
}
