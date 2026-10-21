#include "octo_poller.h"

#include <WiFi.h>
#include <OctoPrinter.h>
#include <atomic>
#include <string.h>

#include "const.h"
#include "enclosure_proto.h" // ENC_OCTO_KEY_MAX
#include "secrets.h"

namespace
{
    secrets sec;
    OctoPrinter api(sec.getOctoprintApiKey(), OCTOPRINT_IP, OCTOPRINT_PORT);
    SemaphoreHandle_t snapshotMutex = nullptr;
    OctoSnapshot shared;
    volatile bool hasData = false;
    volatile bool cancelRequested = false;
    // -1 = no request. Atomic, not a plain int: the regulator task publishes a
    // target while the poll task does an exchange(), and a read-then-clear on a
    // plain int could lose a bed-off request that landed between the two.
    // exchange() makes last-write-wins exact.
    std::atomic<int> pendingBedTarget(-1);

    // Pending endpoint change, published by octoPollerSetEndpoint() from the
    // main/web-UI task and consumed by pollTask(). `pending` is the guard: the
    // value is a multi-byte struct, so the task must not read it while the
    // writer is midway through storing it. Set last, cleared first.
    IPAddress pendingHost;
    uint16_t pendingPort = OCTOPRINT_PORT;
    volatile bool pending = false;

    // Pending API key, same publish/consume pattern as the address above.
    //
    // A fixed char[] and NOT a String, deliberately. The `pending` flag only
    // protects against a half-written read if the value is trivially copyable -
    // a String assignment reallocates and would leave the task reading a
    // freed or half-updated buffer with nothing to stop it. Same reason the
    // address is a POD.
    char pendingKey[ENC_OCTO_KEY_MAX] = {0};
    volatile bool pendingKeyFlag = false;
    // Guards the pendingKey copy between this file's writer (called from loop()
    // and the web UI) and applyPendingApiKey() on the poll task.
    portMUX_TYPE keyMux = portMUX_INITIALIZER_UNLOCKED;

    void applyPendingEndpoint()
    {
        if (!pending)
            return;
        // Clear before reading the value: a change published while we are here
        // sets the flag again and is picked up on the next pass, which is the
        // behaviour we want - no update is ever lost, and none is applied twice.
        pending = false;
        api.setEndpoint(pendingHost, pendingPort);
    }

    // Returns true when a new key was applied.
    bool applyPendingApiKey()
    {
        if (!pendingKeyFlag)
            return false;
        // Copied out under a critical section, which the 4-byte endpoint handoff
        // does not need and this one does.
        //
        // A flag-last/cleared-first protocol only prevents a half-written read
        // in the window the flag covers. For a 65-byte value that window is wide
        // enough to tear, and a torn key is the dangerous case: both the old and
        // new key are printable ASCII of a similar length, so a half-updated
        // buffer still passes validation and could be stored - leaving the board
        // permanently unable to authenticate. A brief interrupt mask around a
        // 65-byte copy costs nothing and makes the handoff indivisible.
        char local[ENC_OCTO_KEY_MAX];
        portENTER_CRITICAL(&keyMux);
        pendingKeyFlag = false;
        strncpy(local, pendingKey, ENC_OCTO_KEY_MAX - 1);
        local[ENC_OCTO_KEY_MAX - 1] = '\0';
        portEXIT_CRITICAL(&keyMux);
        api.setApiKey(local);
        return true;
    }

    void publishSnapshot()
    {
        OctoSnapshot copy;
        copy.printing = api.printing();
        copy.paused = api.paused();
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
#if SERIAL_DEBUG
        Serial.println("[octoPoll] task started, begin()...");
#endif
        // The stored key may already be waiting: octoEndpointBegin() runs before
        // octoPollerStart(), and octoEndpointAttachPoller() publishes it either
        // just before or just after this task is created. Applying it here means
        // begin()'s /api/version is authenticated with the right key in the
        // common case rather than with the compiled-in fallback.
        applyPendingApiKey();
        api.begin();
#if SERIAL_DEBUG
        Serial.println("[octoPoll] begin() done");
#endif
        for (;;)
        {
            // Key first: it must be in force before the request below goes out,
            // or this pass would 401 and the snapshot would publish garbage.
            //
            // begin() is re-run after a key change because setApiKey() cleared
            // _doOnce: the cached server/API version strings were fetched with
            // the previous key and must not be reported as the new one's.
            if (applyPendingApiKey())
                api.begin();
            applyPendingEndpoint();
            if (WiFi.status() == WL_CONNECTED)
            {
                if (cancelRequested)
                {
                    cancelRequested = false;
                    const int status = api.cancelJob();
                    Serial.printf("[octoPoll] cancel job HTTP %d\n", status);
                }
                // Bed target for the calibration. Read-and-clear before the
                // request: a request arriving while this one is in flight is a
                // newer target and must win, not be consumed by this pass.
                const int bedTarget = pendingBedTarget.exchange(-1);
                if (bedTarget >= 0)
                {
                    const int status = api.preheatBed(bedTarget);
                    Serial.printf("[octoPoll] bed target %d C HTTP %d\n", bedTarget, status);
                }
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

void octoPollerRequestCancel()
{
    cancelRequested = true;
}

void octoPollerRequestBed(int targetC)
{
    pendingBedTarget.store(targetC < 0 ? 0 : targetC);
}

void octoPollerSetEndpoint(IPAddress host, uint16_t port)
{
    // Store the value, then raise the flag. pollTask() clears the flag before
    // reading, so a change made mid-apply is not lost - it simply lands on the
    // following pass.
    pendingHost = host;
    pendingPort = port;
    pending = true;
}

void octoPollerSetApiKey(const char *key)
{
    const char *safe = key ? key : "";
    // Truncating copy. An over-long key cannot authenticate as anything, and
    // strncpy would not terminate it - leaving the poll task to send a header
    // value that runs past the end of its own buffer.
    portENTER_CRITICAL(&keyMux);
    strncpy(pendingKey, safe, ENC_OCTO_KEY_MAX - 1);
    pendingKey[ENC_OCTO_KEY_MAX - 1] = '\0';
    // Flag last, cleared first by the poll task: the same ordering the address
    // uses, and for the same reason.
    pendingKeyFlag = true;
    portEXIT_CRITICAL(&keyMux);
}
