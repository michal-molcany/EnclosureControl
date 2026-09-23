#include "enclosure_link.h"

#include <WiFi.h>
#include <esp_now.h>

namespace
{
    uint8_t peerMac[6] = {0, 0, 0, 0, 0, 0};
    bool peerSet = false;
    int peerChannel = 0;
    uint8_t cmdSeq = 0;

    SemaphoreHandle_t snapshotMutex = nullptr;
    QueueHandle_t commandQueue = nullptr;
    EnclosureSnapshot shared;
    volatile bool hasData = false;

    EnclosureCommand lastCmd = {};
    bool hasCmd = false;
    unsigned long lastHeartbeatMs = 0;
    unsigned long lastResyncMs = 0;
    unsigned long lastReminderMs = 0;

    bool isZeroMac(const uint8_t *mac)
    {
        for (int i = 0; i < 6; ++i)
            if (mac[i] != 0)
                return false;
        return true;
    }

    bool addOrUpdatePeer(int channel)
    {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, peerMac, 6);
        peer.channel = (uint8_t)channel;
        peer.encrypt = false;
        if (esp_now_is_peer_exist(peerMac))
        {
            if (esp_now_mod_peer(&peer) != ESP_OK)
                return false;
        }
        else
        {
            if (esp_now_add_peer(&peer) != ESP_OK)
                return false;
        }
        peerChannel = channel;
        return true;
    }

    void sendCmd(const EnclosureCommand &cmd)
    {
        EnclosureCommand out = cmd;
        out.magic = ENC_PROTO_MAGIC;
        out.seq = cmdSeq++;
        esp_now_send(peerMac, (const uint8_t *)&out, sizeof(out));
    }

    void onRecv(const uint8_t * /*mac*/, const uint8_t *data, int len)
    {
        if (len != (int)sizeof(EnclosureTelemetry))
            return;
        const EnclosureTelemetry *tele = (const EnclosureTelemetry *)data;
        if (tele->magic != ENC_PROTO_MAGIC)
            return;
        if (xSemaphoreTake(snapshotMutex, 0) != pdTRUE)
            return;
        shared.louvrePct = tele->louvrePct;
        shared.fanPct = tele->fanPct;
        shared.chamberCx10 = tele->chamberCx10;
        shared.state = tele->state;
        shared.mode = tele->mode;
        shared.seq = tele->seq;
        shared.rxMs = millis();
        shared.valid = true;
        xSemaphoreGive(snapshotMutex);
        hasData = true;
    }

    void linkTask(void *)
    {
        if (esp_now_init() != ESP_OK)
        {
            Serial.println("[ENC-LINK] esp_now_init FAILED");
            vTaskDelete(nullptr);
            return;
        }
        esp_now_register_recv_cb(onRecv);
        if (peerSet)
            addOrUpdatePeer(WiFi.channel());

        for (;;)
        {
            EnclosureCommand cmd;
            bool fresh = false;
            while (xQueueReceive(commandQueue, &cmd, 0) == pdTRUE)
            {
                lastCmd = cmd;
                hasCmd = true;
                fresh = true;
            }
            unsigned long now = millis();
            // Unmissable pairing reminder while transmit is disabled.
            if (!peerSet && (now - lastReminderMs >= 10000UL))
            {
                lastReminderMs = now;
                Serial.print("[ENC-LINK] own MAC ");
                Serial.print(WiFi.macAddress());
                Serial.print(" | ip ");
                Serial.print(WiFi.localIP());
                Serial.print(" ch ");
                Serial.print(WiFi.channel());
                Serial.println(" | set ENC_PEER_MAC to enable TX");
            }
            if (peerSet && hasCmd)
            {
                if (fresh || (now - lastHeartbeatMs >= ENC_CMD_HEARTBEAT_INTERVAL_MS))
                {
                    lastHeartbeatMs = now;
                    sendCmd(lastCmd);
                }
            }
            if (peerSet && (now - lastResyncMs >= ENC_CHANNEL_RESYNC_MS))
            {
                lastResyncMs = now;
                int ch = WiFi.channel();
                if (ch != peerChannel)
                    addOrUpdatePeer(ch);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
} // namespace

void enclosureLinkStart(const uint8_t *peerMacIn)
{
    Serial.print("[ENC-LINK] own MAC: ");
    Serial.println(WiFi.macAddress());
    memcpy(peerMac, peerMacIn, 6);
    peerSet = !isZeroMac(peerMac);
    if (!peerSet)
        Serial.println("[ENC-LINK] no peer MAC set - receive-only (set ENC_PEER_MAC)");

    snapshotMutex = xSemaphoreCreateMutex();
    commandQueue = xQueueCreate(8, sizeof(EnclosureCommand));
    xTaskCreatePinnedToCore(linkTask, "encLink", 8192, nullptr, 1, nullptr, 0);
}

bool enclosureLinkCopy(EnclosureSnapshot &out)
{
    if (!snapshotMutex)
        return false;
    if (xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(20)) != pdTRUE)
        return false;
    out = shared;
    xSemaphoreGive(snapshotMutex);
    return true;
}

bool enclosureLinkHasData()
{
    return hasData;
}

void enclosureLinkSend(const EnclosureCommand &cmd)
{
    if (commandQueue)
        xQueueSend(commandQueue, &cmd, 0);
}
