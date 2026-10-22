#include "poller.h"

#include <WiFi.h>
#include <OctoPrinter.h>
#include <Preferences.h>
#include <string.h>

#include "const.h"
#include "enclosure_proto.h" // ENC_OCTO_KEY_MAX, ENC_KEYFLAG_*
#include "secrets.h"

namespace
{
    // Forward-declared because applyPendingEndpoint() below takes it to publish
    // the address bookkeeping atomically for the UI reader, while its actual
    // definition sits further down with the rest of the shared state.
    SemaphoreHandle_t snapshotMutex = nullptr;

    secrets sec;
    OctoPrinter api(sec.getOctoprintApiKey(), OCTOPRINT_IP, OCTOPRINT_PORT);

    // The compiled-in key, read once and lazily. Not in a static initialiser:
    // it builds a String, and a global String constructed before main() runs is a
    // static-init hazard for no benefit.
    String compiledKeyCache;
    bool compiledKeyRead = false;

    String compiledKey()
    {
        if (!compiledKeyRead)
        {
            compiledKeyCache = sec.getOctoprintApiKey();
            compiledKeyRead = true;
        }
        return compiledKeyCache;
    }

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

    // ---------------------------------------------------------------------
    // OctoPrint API key, as told to us by the C3.
    //
    // Same ownership as the address: the C3 owns the setting (it has the web
    // UI), this board follows it. Stored for the same reason - this board can
    // be power-cycled while the C3 is off, and a reverted key would look
    // exactly like "OctoPrint is down" until the link came back.
    //
    // Its own namespace, because this board also keeps the ADDRESS in
    // "octoend" and clearEndpoint() wipes that whole namespace; sharing one
    // would make a reset of one setting destroy the other.
    // ---------------------------------------------------------------------
    const char *OCTO_KEY_NS = "octokey";
    const char *OCTO_KEY_KEY = "apikey";

    String currentKey;
    bool apiKeyOverridden = false;

    // A fixed char[] and NOT a String, for the same reason pendingHost is a POD:
    // the flag-last/cleared-first protocol only prevents a half-written read if
    // the value is trivially copyable. A String assignment reallocates.
    char pendingKey[ENC_OCTO_KEY_MAX] = {0};
    uint8_t pendingKeyFlags = 0;
    volatile bool pendingKeyFlag = false;
    // Guards the pendingKey copy between this file's writer (called from the
    // link task, on both the ESP-NOW and HTTP paths) and applyPendingApiKey() on
    // the poll task.
    portMUX_TYPE keyMux = portMUX_INITIALIZER_UNLOCKED;

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
    //
    // Forward-declared because it validates the stored API key too, and
    // plausibleKey() is defined further down with the other validators.
    bool plausibleKey(const char *key);

    void loadEndpoint()
    {
        currentHost.fromString(OCTOPRINT_IP);
        currentPort = OCTOPRINT_PORT;
        endpointOverridden = false;

        // The API key, same override-or-default rule as the address. Applied to
        // the client for the same reason as below - the OctoPrinter instance is
        // constructed at static-init with the compiled-in values, so recording a
        // stored value without applying it leaves this board using the old one
        // for the whole session.
        currentKey = "";
        apiKeyOverridden = false;
        // Opened READ-WRITE, unlike the address namespace below. A read-only
        // open of a namespace that does not exist yet fails with
        // nvs_open failed: NOT_FOUND and logs an ESP_ERROR every boot - which is
        // exactly the case here until the C3 first sends a key, since a board
        // with no override never writes this namespace at all. Read-write creates
        // it quietly and costs nothing.
        if (octoPrefs.begin(OCTO_KEY_NS, false))
        {
            // isKey() first: getString() on an absent key logs
            // "nvs_get_str len fail: apikey NOT_FOUND" at error level, which is
            // exactly the state of a board the C3 has never sent a key to. The
            // answer we want ("nothing stored") is not an error, so it should not
            // be reported as one.
            if (octoPrefs.isKey(OCTO_KEY_KEY))
            {
                const String stored = octoPrefs.getString(OCTO_KEY_KEY, "");
                if (plausibleKey(stored.c_str()))
                {
                    currentKey = stored;
                    apiKeyOverridden = true;
                    api.setApiKey(currentKey.c_str());
                }
            }
            octoPrefs.end();
        }

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
                // MUST reach the client, not just the bookkeeping. The instance
                // is constructed at static-init time with the compiled-in
                // OCTOPRINT_IP, so recording the stored value without applying it
                // leaves this board polling the compiled default forever - which
                // is exactly what happened: it booted, printed the stored
                // address, then failed every connection to an address nobody
                // had asked it to use.
                //
                // Harmless when it equals the compiled default: setEndpoint()
                // returns early if nothing changed.
                api.setEndpoint(currentHost, currentPort);
                Serial.print(F("[POLL] endpoint "));
                Serial.print(currentHost);
                Serial.print(':');
                Serial.println(currentPort);
            }
        }
    }

    // Is this a usable OctoPrint address at all?
    //
    // Everything arriving in a telemetry packet is UNTRUSTED. ESP-NOW has no
    // authentication, and in receive-only bootstrap mode (all-zero peer MAC) this
    // board accepts packets from any sender on the channel - so a corrupt frame,
    // a neighbouring board, or anyone with a radio can put four arbitrary bytes
    // in here.
    //
    // Persisting garbage is not cosmetic: it survives every reboot, so one bad
    // frame would point this board permanently at an address that does not
    // exist, and the symptom (no printer data) is identical to OctoPrint being
    // down. Validate before storing, and store nothing on failure.
    // Is this a usable OctoPrint API key at all?
    //
    // Untrusted for the same reason the address is: this board accepts frames
    // from any sender in receive-only bootstrap mode, so four arbitrary bytes -
    // or here, up to 64 arbitrary ones - can arrive from a neighbouring board
    // or anyone with a radio.
    //
    // The failure this prevents is specific and bad. The key is interpolated
    // raw into an "X-Api-Key:" header line by the OctoPrinter client, so a value
    // containing CR or LF would end that line early and turn the rest of the
    // value into headers sent to the OctoPrint server. Anything unprintable is
    // rejected rather than sanitised: a key is machine-generated ASCII, so there
    // is no legitimate value this would refuse.
    //
    // Rejecting (rather than storing and hoping) also means a corrupt frame
    // cannot leave this board permanently unable to authenticate, which would
    // survive every reboot and be indistinguishable from a wrong key by mistake.
    bool plausibleKey(const char *key)
    {
        if (!key || !*key)
            return false;
        const size_t n = strnlen(key, ENC_OCTO_KEY_MAX);
        if (n == 0 || n > ENC_OCTO_KEY_MAX - 1)
            return false;
        for (size_t i = 0; i < n; ++i)
        {
            const unsigned char c = (unsigned char)key[i];
            if (c < 0x20 || c > 0x7E)
                return false;
        }
        return true;
    }

    void applyPendingApiKey()
    {
        if (!pendingKeyFlag)
            return;
        // Snapshotted under a critical section. A flag-last/cleared-first
        // protocol does not make a 65-byte handoff indivisible, and this one
        // genuinely has to be: both the old and new key are printable ASCII of a
        // similar length, so a torn read would still pass plausibleKey() and
        // could be persisted - leaving the display unable to authenticate until
        // someone re-entered the key by hand. A brief interrupt mask around a
        // 65-byte copy costs nothing.
        char key[ENC_OCTO_KEY_MAX];
        portENTER_CRITICAL(&keyMux);
        pendingKeyFlag = false;
        const uint8_t flags = pendingKeyFlags;
        strncpy(key, pendingKey, ENC_OCTO_KEY_MAX - 1);
        key[ENC_OCTO_KEY_MAX - 1] = '\0';
        portEXIT_CRITICAL(&keyMux);

        // 0 means "the C3 had nothing to say". NOT a reset: the C3 omits the key
        // from most frames precisely because it repeats it only after a change,
        // and treating an absent key as "clear" would revert this board to its
        // own compiled-in key once per second.
        if (flags == 0)
            return;

        if (flags & ENC_KEYFLAG_CLEAR)
        {
            // Follow the C3 back to our compiled default, so a "reset" in the
            // web UI resets both boards.
            if (!apiKeyOverridden && currentKey == String())
                return; // already on our default; nothing to do
            apiKeyOverridden = false;
            currentKey = "";
            if (octoPrefs.begin(OCTO_KEY_NS, false))
            {
                octoPrefs.clear();
                octoPrefs.end();
            }
            api.setApiKey(compiledKey().c_str());
            Serial.println(F("[POLL] api key override cleared by C3"));
            return;
        }

        if ((flags & ENC_KEYFLAG_PRESENT) == 0)
            return; // some flag combination nobody defined - ignore it

        if (!plausibleKey(key))
        {
            Serial.println(F("[POLL] api key REJECTED (implausible characters)"));
            return;
        }

        // Re-assert even when unchanged: setApiKey() returns immediately when
        // the client already holds this key, so it costs nothing and makes the
        // client and the bookkeeping self-heal instead of depending on the two
        // having been updated together.
        api.setApiKey(key);

        if (apiKeyOverridden && currentKey == String(key))
            return; // unchanged: no NVS write, no log spam every refresh frame

        apiKeyOverridden = true;
        currentKey = String(key);
        if (octoPrefs.begin(OCTO_KEY_NS, false))
        {
            octoPrefs.putString(OCTO_KEY_KEY, currentKey);
            octoPrefs.end();
        }
        Serial.print(F("[POLL] api key from C3 ("));
        Serial.print(currentKey.length());
        Serial.println(F(" chars)"));
    }

    bool plausibleEndpoint(IPAddress ip, uint16_t port)
    {
        if (port == 0)
            return false;
        const uint8_t first = ip[0];
        if (first == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255)
            return false; // broadcast
        if (first >= 224)
            return false; // multicast (224/4) and reserved space
        if (first == 127)
            return false; // loopback: this board reaches OctoPrint over the LAN
        return true;
    }

    // Runs on the poll task, at the top of its loop. Applies and persists in
    // one place so the stored value can never disagree with the value in force.
    void applyPendingEndpoint()
    {
        if (!pending)
            return;
        pending = false;

        // Held across the bookkeeping below, and released before the slow calls.
        //
        // pollerCopyOctoInfo() reads currentHost/currentPort/endpointOverridden
        // from the UI task, and IPAddress is 4 bytes - so an unlocked read can
        // splice two different addresses together and put a nonsense IP on the
        // info screen. Cheap here: this runs at most once per *changed* value,
        // not once per poll, and none of storeEndpoint()/setEndpoint() re-enter
        // this mutex.
        struct BookkeepingLock
        {
            SemaphoreHandle_t m;
            explicit BookkeepingLock(SemaphoreHandle_t mutex) : m(mutex)
            {
                if (m)
                    xSemaphoreTake(m, portMAX_DELAY);
            }
            ~BookkeepingLock()
            {
                if (m)
                    xSemaphoreGive(m);
            }
        } bookKeeping(snapshotMutex);

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

        // Last gate before anything is written to flash or handed to the poller.
        if (!plausibleEndpoint(ip, port))
        {
            Serial.print(F("[POLL] endpoint REJECTED "));
            Serial.print(ip);
            Serial.print(':');
            Serial.println(port);
            return;
        }

        // Re-assert even when the value is unchanged. setEndpoint() returns
        // immediately when the client already points here, so this costs
        // nothing - and it makes the client and the bookkeeping self-heal
        // rather than depending on the two having been updated together.
        api.setEndpoint(ip, port);

        if (endpointOverridden && ip == currentHost && port == currentPort)
            return; // unchanged: no NVS write, no log spam every single frame

        endpointOverridden = true;
        currentHost = ip;
        currentPort = port;
        storeEndpoint(ip, port);
        Serial.print(F("[POLL] endpoint from C3 "));
        Serial.print(ip);
        Serial.print(':');
        Serial.println(port);
    }
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
        uint32_t updatedMs = 0;
        uint32_t lastAttemptMs = 0;
        bool hasUpdated = false;
        bool busy = false;
        bool error = false;
    };
    FileListCache *sharedFiles = nullptr;
    uint32_t filesVersion = 0;

    // One-entry detail cache for the info panel. Heap-allocated for the same
    // reason as the list: another PrintFile in static DRAM would not fit.
    struct FileInfoCache
    {
        PrintFile file;
        uint32_t version = 0;
        bool busy = false;
        bool ready = false;
        bool error = false;
    };
    FileInfoCache *sharedInfo = nullptr;

    // Staged path for PRINT_FILE / FILE_INFO. Written by the UI task just before
    // it enqueues one of those commands, read by the poll task after dequeuing
    // it, so the queue itself provides the happens-after ordering. The two
    // commands are never queued together (the info panel and print confirm are
    // separate screens), so one buffer is enough.
    char stagedPath[PRINT_FILE_PATH_LEN] = {};
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
            sharedFiles->lastAttemptMs = millis();
            xSemaphoreGive(snapshotMutex);
        }
    }

    void markInfoBusy()
    {
        if (snapshotMutex && sharedInfo &&
            xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(20)) == pdTRUE)
        {
            sharedInfo->busy = true;
            sharedInfo->error = false;
            xSemaphoreGive(snapshotMutex);
        }
    }

    // Copies one parsed entry into the fixed-size cache shape. Both paths use
    // this so the field list only exists once.
    void copyFile(const OctoFile &src, PrintFile &dst)
    {
        strncpy(dst.path, src.path, PRINT_FILE_PATH_LEN - 1);
        dst.path[PRINT_FILE_PATH_LEN - 1] = '\0';
        strncpy(dst.name, src.name, PRINT_FILE_NAME_LEN - 1);
        dst.name[PRINT_FILE_NAME_LEN - 1] = '\0';
        dst.date = src.date;
        dst.size = src.size;
        memcpy(dst.material, src.material, sizeof(dst.material));
        dst.estimatedPrintTime = src.estimatedPrintTime;
        dst.filamentLengthMm = src.filamentLengthMm;
        dst.filamentVolumeCm3 = src.filamentVolumeCm3;
        dst.dimX = src.dimX;
        dst.dimY = src.dimY;
        dst.dimZ = src.dimZ;
        dst.averagePrintTime = src.averagePrintTime;
        dst.printsSuccess = src.printsSuccess;
        dst.printsFailure = src.printsFailure;
        dst.layerHeightMm = src.layerHeightMm;
        dst.nozzleMm = src.nozzleMm;
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
                // Transient heap, not static and not poll-task stack (the
                // entries are ~576 B, against an 8 KB task stack; static would
                // overflow the DRAM segment - see above).
                //
                // Note the cost here is NOT proportional to MAX_PRINT_FILES:
                // fetchFiles still streams OctoPrint's whole recursive listing.
                // It now keeps only the fields the rows draw, which makes the
                // parse cheap; this allocation is just the destination.
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
                        // A periodic check that finds the same list must not
                        // advance the version: the UI would rebuild the rows and
                        // throw away the user's selection every five minutes.
                        bool changed = found != sharedFiles->count;
                        for (int i = 0; i < found && !changed; ++i)
                        {
                            changed = strcmp(sharedFiles->files[i].path, scratch[i].path) != 0 ||
                                      sharedFiles->files[i].date != scratch[i].date ||
                                      sharedFiles->files[i].size != scratch[i].size ||
                                      strcmp(sharedFiles->files[i].name, scratch[i].name) != 0;
                        }
                        sharedFiles->count = (uint8_t)found;
                        for (int i = 0; i < found; ++i)
                            copyFile(scratch[i], sharedFiles->files[i]);
                        sharedFiles->error = false;
                        sharedFiles->updatedMs = millis();
                        sharedFiles->hasUpdated = true;
                        if (changed)
                            sharedFiles->version = ++filesVersion;
                        Serial.printf("[POLL] files refresh ok count=%d changed=%d version=%u\n",
                                      found, changed, filesVersion);
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
            case FILE_INFO:
            {
                if (!sharedInfo || stagedPath[0] == '\0')
                    break;
                Serial.printf("[POLL] file info start %s\n", stagedPath);
                OctoFile scratch;
                OctoPrinter::FetchDiag diag = OctoPrinter::FETCH_OK;
                uint8_t pcode = 0;
                const int ok = api.fetchFileInfo(stagedPath, scratch, diag, pcode);
                if (xSemaphoreTake(snapshotMutex, portMAX_DELAY) == pdTRUE)
                {
                    if (ok > 0)
                    {
                        copyFile(scratch, sharedInfo->file);
                        sharedInfo->ready = true;
                        sharedInfo->error = false;
                        Serial.printf("[POLL] file info ok %s\n", stagedPath);
                    }
                    else
                    {
                        // Keep whatever was cached for the previous file; ready
                        // false is what tells the UI this request failed.
                        sharedInfo->ready = false;
                        sharedInfo->error = true;
                        Serial.printf("[POLL] file info FAILED diag=%u code=%u\n",
                                      (unsigned)diag, pcode);
                    }
                    sharedInfo->busy = false;
                    sharedInfo->version++;
                    xSemaphoreGive(snapshotMutex);
                }
                break;
            }
            case PRINT_FILE:
            {
                if (stagedPath[0] == '\0')
                    break;
                // Second guard behind the idle-only entry: a job that started
                // elsewhere after the confirm opened must not be overridden.
                if (api.printing() || api.paused())
                {
                    Serial.println("[POLL] print REJECTED job active");
                    break;
                }
                Serial.printf("[POLL] print %s -> HTTP %d\n", stagedPath,
                              api.selectAndPrint(stagedPath));
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
        // Stamped here, on the poll task, so it means "this task last had data at
        // T" - not "the UI last copied it". That is what makes it usable as a
        // staleness signal.
        copy.updatedMs = millis();
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
            // Key first: it must be in force before the request below goes out,
            // or this pass would 401 and publish a snapshot with no readings.
            applyPendingApiKey();
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
    sharedInfo = static_cast<FileInfoCache *>(calloc(1, sizeof(FileInfoCache)));
    if (!sharedInfo)
        Serial.println("[POLL] file info cache alloc FAILED; details will report errors");
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

bool pollerCopyOctoInfo(PollerOctoInfo &out)
{
    if (!snapshotMutex)
        return false;
    // Same mutex as the snapshot, and for the same reason: currentHost,
    // currentPort, endpointOverridden, apiKeyOverridden and `shared` are all
    // written by the poll task and read here from the UI task. Reading them
    // directly would be an unsynchronised cross-task read of a multi-byte struct.
    if (xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return false;
    out.host = currentHost;
    out.port = currentPort;
    out.endpointOverridden = endpointOverridden;
    out.apiKeyOverridden = apiKeyOverridden;
    out.printing = shared.printing;
    out.paused = shared.paused;
    out.valid = shared.valid;
    out.lastUpdateMs = shared.updatedMs;
    xSemaphoreGive(snapshotMutex);
    return true;
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

void pollerSetApiKey(const char *key, uint8_t flags)
{
    if (flags == 0)
        return; // nothing to say - see applyPendingApiKey()
    // Truncating copy, always NUL-terminated: strncpy alone would leave an
    // over-long value unterminated, and the poll task would then read past the
    // end of this buffer when it validates and sends it.
    const char *safe = key ? key : "";
    // Truncating copy, always NUL-terminated: strncpy alone would leave an
    // over-long value unterminated, and the poll task would then read past the
    // end of this buffer when it validates and sends it.
    portENTER_CRITICAL(&keyMux);
    strncpy(pendingKey, safe, ENC_OCTO_KEY_MAX - 1);
    pendingKey[ENC_OCTO_KEY_MAX - 1] = '\0';
    pendingKeyFlags = flags;
    // Flag last, cleared first by the poll task: same ordering as the address.
    pendingKeyFlag = true;
    portEXIT_CRITICAL(&keyMux);
}

bool pollerApiKeyOverridden()
{
    return apiKeyOverridden;
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

bool pollerFilesShouldRefresh(uint32_t ttlMs, uint32_t retryMs)
{
    if (!snapshotMutex || !sharedFiles)
        return false;
    if (xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return false;
    bool refresh = false;
    if (!sharedFiles->busy)
    {
        if (sharedFiles->hasUpdated)
            refresh = (uint32_t)(millis() - sharedFiles->updatedMs) >= ttlMs;
        else
            refresh = sharedFiles->lastAttemptMs == 0 ||
                      (uint32_t)(millis() - sharedFiles->lastAttemptMs) >= retryMs;
    }
    xSemaphoreGive(snapshotMutex);
    return refresh;
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

bool pollerCopyFileByPath(const char *path, PrintFile &out)
{
    if (!snapshotMutex || !sharedFiles || !path || !*path)
        return false;
    if (xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return false;
    bool ok = false;
    for (uint8_t i = 0; i < sharedFiles->count; ++i)
    {
        if (strcmp(sharedFiles->files[i].path, path) == 0)
        {
            out = sharedFiles->files[i];
            ok = true;
            break;
        }
    }
    xSemaphoreGive(snapshotMutex);
    return ok;
}

bool pollerCopyFileInfo(PrintFile &out, uint32_t &version, bool &busy,
                        bool &ready, bool &error)
{
    if (!snapshotMutex || !sharedInfo)
        return false;
    if (xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return false;
    out = sharedInfo->file;
    version = sharedInfo->version;
    busy = sharedInfo->busy;
    ready = sharedInfo->ready;
    error = sharedInfo->error;
    xSemaphoreGive(snapshotMutex);
    return true;
}

void pollerRequestFilesRefresh()
{
    // Optimistic busy so the view shows "Loading..." at once instead of
    // waiting for the next poll cycle to pick up the queued command.
    markFilesBusy();
    pollerSendCommand({PollerCommand::Job, REFRESH_FILES});
}

void pollerRequestFileInfo(const char *path)
{
    if (!path || !*path)
        return;
    markInfoBusy();
    strncpy(stagedPath, path, PRINT_FILE_PATH_LEN - 1);
    stagedPath[PRINT_FILE_PATH_LEN - 1] = '\0';
    pollerSendCommand({PollerCommand::Job, FILE_INFO});
}

void pollerRequestFilePrint(const char *path)
{
    if (!path || !*path)
        return;
    strncpy(stagedPath, path, PRINT_FILE_PATH_LEN - 1);
    stagedPath[PRINT_FILE_PATH_LEN - 1] = '\0';
    pollerSendCommand({PollerCommand::Job, PRINT_FILE});
}
