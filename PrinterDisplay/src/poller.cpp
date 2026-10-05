#include "poller.h"

#include <WiFi.h>
#include <OctoPrinter.h>

#include "const.h"
#include "secrets.h"

namespace
{
    secrets sec;
    OctoPrinter api(sec.getOctoprintApiKey(), OCTOTPRINT_IP, OCTOTPRINT_PORT);
    SemaphoreHandle_t snapshotMutex = nullptr;
    QueueHandle_t commandQueue = nullptr;
    PrinterSnapshot shared;
    volatile bool hasData = false;

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
        api.begin();
        for (;;)
        {
PollerCommand cmd = {};
        while (xQueueReceive(commandQueue, &cmd, 0) == pdTRUE)
                executeCommand(cmd);
            if (WiFi.status() == WL_CONNECTED)
            {
                api.update();
                publishSnapshot();
            }
            vTaskDelay(pdMS_TO_TICKS(API_REFRESH_TIME));
        }
    }
} // namespace

void pollerStart()
{
    snapshotMutex = xSemaphoreCreateMutex();
    commandQueue = xQueueCreate(8, sizeof(PollerCommand));
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

void pollerSendCommand(const PollerCommand &cmd)
{
    if (commandQueue)
        xQueueSend(commandQueue, &cmd, 0);
}
