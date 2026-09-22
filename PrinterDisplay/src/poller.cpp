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

    void executeCommand(int code)
    {
        switch (code)
        {
        case PREHEAT_215:
        case PREHEAT_230:
            api.preheat(code);
            break;
        case PREHEAT_OFF:
            api.preheatOff();
            break;
        case PAUSE_PRINT_BUTTON:
            api.pauseJob();
            break;
        case RESUME_PRINT_BUTTON:
            api.resumeJob();
            break;
        case STOP_PRINT_BUTTON:
            api.cancelJob();
            break;
        default:
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
            int code = 0;
            while (xQueueReceive(commandQueue, &code, 0) == pdTRUE)
                executeCommand(code);
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
    commandQueue = xQueueCreate(8, sizeof(int));
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

void pollerSendCommand(int code)
{
    if (commandQueue)
        xQueueSend(commandQueue, &code, 0);
}
