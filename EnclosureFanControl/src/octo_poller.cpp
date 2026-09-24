#include "octo_poller.h"

#include <WiFi.h>
#include <OctoPrinter.h>

#include "const.h"
#include "secrets.h"

namespace
{
    secrets sec;
#ifdef OCTOPRINT_IP
    OctoPrinter api(sec.getOctoprintApiKey(), OCTOPRINT_IP, OCTOPRINT_PORT);
#else
    OctoPrinter api(sec.getOctoprintApiKey(), OCTOTPRINT_IP, OCTOTPRINT_PORT);
#endif
    SemaphoreHandle_t snapshotMutex = nullptr;
    OctoSnapshot shared;
    volatile bool hasData = false;

    void publishSnapshot()
    {
        OctoSnapshot copy;
        copy.printing = api.printing();
        copy.filament = api.filamentName();
        copy.toolActual = api.toolActual();
        copy.bedActual = api.bedActual();
        copy.chamberActual = api.chamberActual();
        copy.closedOrError = api.closedOrError();
        copy.valid = true;
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
        Serial.println("[octoPoll] task started, begin()...");
        api.begin();
        Serial.println("[octoPoll] begin() done");
        for (;;)
        {
            if (WiFi.status() == WL_CONNECTED)
            {
                api.update();
                publishSnapshot();
            }
            vTaskDelay(pdMS_TO_TICKS(OCTO_POLL_INTERVAL_MS));
        }
    }
} // namespace

void octoPollerStart()
{
    if (snapshotMutex == nullptr)
        snapshotMutex = xSemaphoreCreateMutex();
    // ESP32-C3 is single-core: no Core-1 pinning (CYD uses core 0).
    xTaskCreate(pollTask, "octoPoll", 8192, nullptr, 1, nullptr);
}

bool octoPollerCopy(OctoSnapshot &out)
{
    if (!snapshotMutex)
        return false;
    if (xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return false;
    out = shared;
    xSemaphoreGive(snapshotMutex);
    return true;
}

bool octoPollerHasData()
{
    return hasData;
}
