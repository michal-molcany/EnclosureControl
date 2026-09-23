#include "espnow_link.h"
#include "temperature_regulation.h"

#include <WiFi.h>
#include <esp_now.h>

namespace
{
    uint8_t peerMac[6] = {0, 0, 0, 0, 0, 0};
    bool peerSet = false;
    int peerChannel = 0;
    bool linkReady = false;

    SemaphoreHandle_t rxMutex = nullptr;
    EnclosureCommand rxSlot = {};
    volatile bool rxFlag = false;

    uint8_t teleSeq = 0;
    unsigned long lastTeleMs = 0;
    unsigned long lastResyncMs = 0;
    unsigned long lastReminderMs = 0;
    uint32_t txOk = 0;
    uint32_t txFail = 0;

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

    void onSent(const uint8_t * /*mac*/, esp_now_send_status_t status)
    {
        if (status == ESP_NOW_SEND_SUCCESS)
            ++txOk;
        else
            ++txFail;
    }

    void onRecv(const uint8_t * /*mac*/, const uint8_t *data, int len)
    {
        if (len != (int)sizeof(EnclosureCommand))
            return;
        if (rxMutex && xSemaphoreTake(rxMutex, 0) == pdTRUE)
        {
            memcpy(&rxSlot, data, sizeof(rxSlot));
            rxFlag = true;
            xSemaphoreGive(rxMutex);
        }
    }
} // namespace

void espnowLinkBegin(const uint8_t *peerMacIn){
    Serial.print("[LINK] own MAC: ");
    Serial.println(WiFi.macAddress());

    memcpy(peerMac, peerMacIn, 6);
    peerSet = !isZeroMac(peerMac);

    if (esp_now_init() != ESP_OK)
    {
        Serial.println("[LINK] esp_now_init FAILED");
        return;
    }
    esp_now_register_send_cb(onSent);
    esp_now_register_recv_cb(onRecv);
    rxMutex = xSemaphoreCreateMutex();

    if (peerSet)
    {
        int ch = WiFi.channel();
        if (addOrUpdatePeer(ch))
        {
            Serial.print("[LINK] peer added on channel ");
            Serial.println(ch);
        }
        else
        {
            Serial.println("[LINK] peer add FAILED");
        }
    }
    else
    {
        Serial.println("[LINK] no peer MAC set - receive-only (set ENC_PEER_MAC)");
    }
    linkReady = true;
}

void espnowLinkPoll(TemperatureRegulation &reg, double chamberTemp, bool chamberValid)
{
    if (!linkReady)
        return;

    // Drain inbound command (copied by the WiFi-task callback).
    if (rxMutex && rxFlag && xSemaphoreTake(rxMutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        bool pending = rxFlag;
        EnclosureCommand cmd = rxSlot;
        rxFlag = false;
        xSemaphoreGive(rxMutex);
        if (pending)
            reg.applyRemoteCommand(cmd);
    }

    unsigned long now = millis();

    // Unmissable pairing reminder while transmit is disabled: boot lines are
    // easily missed when the monitor opens late over USB-CDC.
    if (!peerSet && (now - lastReminderMs >= 10000UL))
    {
        lastReminderMs = now;
        Serial.print("[LINK] own MAC ");
        Serial.print(WiFi.macAddress());
        Serial.print(" | ip ");
        Serial.print(WiFi.localIP());
        Serial.print(" ch ");
        Serial.print(WiFi.channel());
        Serial.println(" | set ENC_PEER_MAC to enable TX");
    }

    // ESP-NOW only works on the STA channel; follow AP channel changes.
    if (peerSet && (now - lastResyncMs >= ENC_CHANNEL_RESYNC_MS))
    {
        lastResyncMs = now;
        int ch = WiFi.channel();
        if (ch != peerChannel)
        {
            if (addOrUpdatePeer(ch))
            {
                Serial.print("[LINK] peer channel resync -> ");
                Serial.println(ch);
            }
        }
    }

    // Telemetry (loop runs ~2s, so this fires about every other second).
    if (peerSet && (now - lastTeleMs >= ENC_TELEMETRY_INTERVAL_MS))
    {
        lastTeleMs = now;
        EnclosureTelemetry tele = {};
        tele.magic = ENC_PROTO_MAGIC;
        tele.seq = teleSeq++;
        tele.louvrePct = reg.louvrePercent();
        tele.fanPct = reg.fanPercent();
        tele.chamberCx10 = chamberValid ? (int16_t)(chamberTemp * 10.0) : ENC_CHAMBER_INVALID;
        tele.state = reg.stateCode();
        tele.mode = reg.controlModeCode();
        if (esp_now_send(peerMac, (const uint8_t *)&tele, sizeof(tele)) != ESP_OK)
            ++txFail;
    }
}

bool espnowLinkHasPeer()
{
    return peerSet;
}
