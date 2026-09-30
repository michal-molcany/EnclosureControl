#include "enclosure_link.h"
#include "enclosure_discovery.h"
#include "enclosure_channel.h"
#include "const.h"

#include <WiFi.h>
#include <esp_now.h>

namespace
{
    const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    EnclosureChannelPin channelPin;

    // Compiled-in peer. Discovery may replace this at runtime, so the address
    // actually used for unicast lives in activeMac.
    uint8_t peerMac[6] = {0, 0, 0, 0, 0, 0};
    uint8_t activeMac[6] = {0, 0, 0, 0, 0, 0};
    bool peerSet = false;  // a usable unicast peer is known
    bool learned = false;  // it came from discovery rather than const.h
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
    unsigned long lastDiagMs = 0;
    unsigned long lastHelloMs = 0;
    unsigned long lastDiscReportMs = 0;

    // Discovery state.
    uint32_t hellosRx = 0;
    uint8_t peerDiscChannel = 0;
    uint8_t pendingHelloMac[6] = {0};
    volatile bool pendingHello = false;

    // Diagnostics. A silent link is the hardest failure to chase, so every
    // stage that can silently eat a packet has its own counter.
    uint32_t rxOk = 0;
    uint32_t txOk = 0;
    uint32_t txFail = 0;
    uint32_t dropLen = 0;
    uint32_t dropMagic = 0;
    uint32_t dropSender = 0;
    uint32_t dropBusy = 0;
    uint8_t lastDrop = ENC_DROP_NONE;
    unsigned long lastRxMs = 0;
    unsigned long lastTxMs = 0;

    const char *dropName(uint8_t reason)
    {
        switch (reason)
        {
        case ENC_DROP_LEN:
            return "len";
        case ENC_DROP_MAGIC:
            return "magic";
        case ENC_DROP_SENDER:
            return "sender";
        case ENC_DROP_BUSY:
            return "busy";
        default:
            return "-";
        }
    }

    void noteDrop(uint8_t reason)
    {
        lastDrop = reason;
        switch (reason)
        {
        case ENC_DROP_LEN:
            ++dropLen;
            break;
        case ENC_DROP_MAGIC:
            ++dropMagic;
            break;
        case ENC_DROP_SENDER:
            ++dropSender;
            break;
        case ENC_DROP_BUSY:
            ++dropBusy;
            break;
        default:
            break;
        }
    }

    bool macEquals(const uint8_t *a, const uint8_t *b)
    {
        return memcmp(a, b, 6) == 0;
    }

    bool isZeroMac(const uint8_t *mac)
    {
        for (int i = 0; i < 6; ++i)
            if (mac[i] != 0)
                return false;
        return true;
    }

    void printMac(const uint8_t *mac)
    {
        for (int i = 0; i < 6; ++i)
        {
            if (i)
                Serial.print(':');
            Serial.printf("%02X", mac[i]);
        }
    }

    // Registers a unicast peer at a channel. Used both for the compiled-in
    // address and for one learned from a discovery hello.
    bool addOrUpdatePeer(const uint8_t *mac, int channel)
    {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, mac, 6);
        peer.channel = (uint8_t)channel;
        peer.encrypt = false;
        if (esp_now_is_peer_exist(mac))
        {
            if (esp_now_mod_peer(&peer) != ESP_OK)
                return false;
        }
        else
        {
            if (esp_now_add_peer(&peer) != ESP_OK)
                return false;
        }
        if (mac == activeMac)
            peerChannel = channel;
        return true;
    }

    // The broadcast peer is what makes discovery possible. Channel 0 means
    // "whatever the STA is on", so it needs no resync.
    void addBroadcastPeer()
    {
        if (esp_now_is_peer_exist(broadcastMac))
            return;
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, broadcastMac, 6);
        peer.channel = 0;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }

    void printMacLn(const uint8_t *mac)
    {
        printMac(mac);
        Serial.println();
    }

    // WiFi.macAddress() and WiFi.BSSID() return a formatted String on the ESP32 and
    // a raw uint8_t* on the ESP32-C3 in arduino-esp32 2.0.x. These must be printed
    // by value, never by casting a String to uint8_t*: that would print the first
    // six ASCII characters of the text and produce an address that looks plausible
    // but is not the board's. Overloads absorb the two return types so one code
    // path is correct on both boards.
    void printAddr(const uint8_t *raw) { printMac(raw); }
    void printAddr(const String &s) { Serial.print(s); }

    // Adopts a peer address learned from a discovery hello. Replaces the
    // compiled-in constant, which is what repairs a hand-copied wrong MAC.
    void adoptLearnedPeer(const uint8_t *mac, uint8_t theirChannel)
    {
        if (macEquals(mac, activeMac))
            return;
        memcpy(activeMac, mac, 6);
        peerSet = true;
        learned = true;
        peerDiscChannel = theirChannel;
        peerChannel = 0;
        addOrUpdatePeer(activeMac, WiFi.channel());
        Serial.print("[ENC-LINK] discovery: peer learned ");
        printMacLn(activeMac);
        if (theirChannel != (uint8_t)WiFi.channel())
        {
            Serial.print("[ENC-LINK] WARNING peer is on channel ");
            Serial.print(theirChannel);
            Serial.print(", we are on ");
            Serial.println(WiFi.channel());
        }
    }

    void sendCmd(const EnclosureCommand &cmd)
    {
        EnclosureCommand out = cmd;
        out.magic = ENC_PROTO_MAGIC;
        out.seq = cmdSeq++;
        lastTxMs = millis();
        esp_now_send(activeMac, (const uint8_t *)&out, sizeof(out));
    }

    void onSent(const uint8_t * /*mac*/, esp_now_send_status_t status)
    {
        if (status == ESP_NOW_SEND_SUCCESS)
            ++txOk;
        else
            ++txFail;
    }

    void onRecv(const uint8_t *mac, const uint8_t *data, int len)
    {
        // Discovery hellos are a different length and a different magic, so
        // they are dispatched before the telemetry checks rather than counted
        // as malformed telemetry.
        uint8_t role = 0;
        uint8_t theirChannel = 0;
        if (enclosureDiscoveryParse(data, len, ENC_ROLE_CYD, &role, &theirChannel))
        {
            ++hellosRx;
            peerDiscChannel = theirChannel;
            // Handled on the link task's next tick rather than here: this runs
            // in the WiFi task and must not call into esp_now peer management.
            memcpy(pendingHelloMac, mac, 6);
            pendingHello = true;
            return;
        }

        if (len != (int)sizeof(EnclosureTelemetry))
        {
            noteDrop(ENC_DROP_LEN);
            return;
        }
        const EnclosureTelemetry *tele = (const EnclosureTelemetry *)data;
        if (tele->magic != ENC_PROTO_MAGIC)
        {
            noteDrop(ENC_DROP_MAGIC);
            return;
        }
        // Only enforced once paired: an all-zero peer MAC is receive-only
        // bootstrap mode, where any sender is by definition acceptable.
        if (peerSet && !macEquals(mac, activeMac))
        {
            noteDrop(ENC_DROP_SENDER);
            return;
        }
        // Bounded rather than immediate: the UI task holds the mutex for up to
        // 20ms, and a zero timeout dropped the whole packet whenever the two
        // collided, which looks exactly like a dead link.
        if (xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(5)) != pdTRUE)
        {
            noteDrop(ENC_DROP_BUSY);
            return;
        }
        shared.louvrePct = tele->louvrePct;
        shared.fanPct = tele->fanPct;
        shared.chamberCx10 = tele->chamberCx10;
        shared.state = tele->state;
        shared.mode = tele->mode;
        shared.seq = tele->seq;
        shared.rxMs = millis();
        shared.valid = true;
        xSemaphoreGive(snapshotMutex);
        lastRxMs = shared.rxMs;
        ++rxOk;
        hasData = true;
    }

    void linkTask(void *)
    {
        if (esp_now_init() != ESP_OK)
        {
            Serial.println("[ENC-LINK] esp_now_init FAILED - link is dead");
            vTaskDelete(nullptr);
            return;
        }
        esp_now_register_send_cb(onSent);
        esp_now_register_recv_cb(onRecv);
        addBroadcastPeer();

        // Single owner for the channel: the link task applies the pin and then
        // keeps asserting it, so main() does not have to know about it.
        channelPin.begin(ENC_WIFI_CHANNEL);
        if (ENC_WIFI_CHANNEL != 0)
        {
            Serial.print("[ENC-LINK] channel pin ");
            Serial.print(enclosurePinChannel(ENC_WIFI_CHANNEL) ? "requested " : "REFUSED ");
            Serial.print(ENC_WIFI_CHANNEL);
            Serial.print(" -> actual ");
            Serial.println(WiFi.channel());
        }
        else
        {
            Serial.print("[ENC-LINK] joined on channel ");
            Serial.println(WiFi.channel());
        }

        if (peerSet)
        {
            const int ch = WiFi.channel();
            if (addOrUpdatePeer(activeMac, ch))
            {
                Serial.print("[ENC-LINK] configured peer ");
                printMacLn(peerMac);
                Serial.print("[ENC-LINK] paired on channel ");
                Serial.println(ch);
            }
            else
            {
                Serial.println("[ENC-LINK] peer add FAILED");
            }
        }
        else
        {
            Serial.println("[ENC-LINK] no ENC_PEER_MAC - relying on discovery");
        }

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

            // Adopt a peer discovered on the air. Done here, not in the
            // callback, because it calls into esp_now peer management.
            if (pendingHello)
            {
                pendingHello = false;
                adoptLearnedPeer(pendingHelloMac, peerDiscChannel);
            }

            // Broadcast our own hello. This is what makes a wrong
            // ENC_PEER_MAC self-correcting, and "no hellos heard" is the
            // positive signal that the boards are on different channels or APs.
            if (now - lastHelloMs >= ENC_DISCOVERY_INTERVAL_MS)
            {
                lastHelloMs = now;
                EnclosureDiscovery hello = {};
                hello.magic = ENC_DISCOVERY_MAGIC;
                hello.role = ENC_ROLE_CYD;
                hello.channel = (uint8_t)WiFi.channel();
                esp_now_send(broadcastMac, (const uint8_t *)&hello, sizeof(hello));
            }

            // Discovery summary, on its own slower cadence so it stays
            // readable next to the link status line.
            if (now - lastDiscReportMs >= ENC_DISCOVERY_REPORT_MS)
            {
                lastDiscReportMs = now;
                Serial.print("[ENC-LINK] discovery hellosRx=");
                Serial.print(hellosRx);
                Serial.print(" we=ch");
                Serial.print(WiFi.channel());
                if (hellosRx == 0)
                {
                    // The single most useful line this firmware can print.
                    // Neither board can hear the other on a different channel,
                    // so the channel is the thing to check first - and the two
                    // boards have to be compared by hand, since neither can
                    // detect a mismatch on its own.
                    Serial.println(" NO PEER HEARD");
                    Serial.print("[ENC-LINK] ACTION both boards must be on the same AP AND channel.");
                    Serial.print(" We are on channel ");
                    Serial.print(WiFi.channel());
                    Serial.println(" - compare with the C3's 'joined on channel' line.");
                }
                else
                {
                    Serial.print(" peer=ch");
                    Serial.print(peerDiscChannel);
                    Serial.print(" mac=");
                    printMacLn(activeMac);
                }
            }

            if (peerSet && hasCmd)
            {
                if (fresh || (now - lastHeartbeatMs >= ENC_CMD_HEARTBEAT_INTERVAL_MS))
                {
                    lastHeartbeatMs = now;
                    sendCmd(lastCmd);
                }
            }
            if (now - lastResyncMs >= ENC_CHANNEL_RESYNC_MS)
            {
                lastResyncMs = now;
                // Pin maintenance runs whether or not a peer is known: a board
                // with no peer is exactly the case a pin exists to rescue, so
                // gating it on peerSet would leave the one situation that needs
                // it unattended.
                channelPin.maintain(now, WiFi.channel(), "[ENC-LINK]");
                if (peerSet && WiFi.channel() != peerChannel)
                    addOrUpdatePeer(activeMac, WiFi.channel());
            }
            // One line per ENC_DIAG_INTERVAL_MS. Reading it:
            //   tx=0/0   -> we have not queued a command yet (UI never sent one)
            //   tx=0/9   -> sends are failing outright (peer absent / off-channel)
            //   rx=0, drop=0, last=-
            //            -> nothing is arriving at all: wrong peer MAC, or the
            //               peer is on another AP or channel
            //   last=sender -> frames arrive but the source MAC is not our peer
            //   last=busy   -> accepted but dropped on the snapshot mutex
            if (peerSet && (now - lastDiagMs >= ENC_DIAG_INTERVAL_MS))
            {
                lastDiagMs = now;
                const uint32_t drops = dropLen + dropMagic + dropSender + dropBusy;
                Serial.print("[ENC-LINK] ch=");
                Serial.print(peerChannel);
                Serial.print(" rx=");
                Serial.print(rxOk);
                Serial.print(" tx=");
                Serial.print(txOk);
                Serial.print('/');
                Serial.print(txFail);
                Serial.print(" drop=");
                Serial.print(drops);
                Serial.print(" last=");
                Serial.print(dropName(lastDrop));
                Serial.print(" age=");
                Serial.print(rxOk ? (now - lastRxMs) : 0);
                Serial.println("ms");
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
} // namespace

void enclosureLinkStart(const uint8_t *peerMacIn)
{
    memcpy(peerMac, peerMacIn, 6);
    memcpy(activeMac, peerMacIn, 6);
    peerSet = !isZeroMac(peerMac);

    // One unconditional boot line, deliberately not behind SERIAL_DEBUG. This is
    // the single most useful line when the link is dead, and the four fields it
    // carries are what localise the failure:
    //   we=ch - the channel this station actually settled on. Two boards on
    //          different channels cannot exchange ESP-NOW frames at all, because
    //          the frames never leave the radio.
    //   bssid - which AP radio this board actually attached to. Same SSID does not
    //          mean same radio: a mesh or extender broadcasts one SSID from
    //          several BSSIDs on different channels. Comparing bssid across the
    //          two boards is what separates "one AP moved channel" (fixable by
    //          ENC_WIFI_CHANNEL) from "two different APs" (not fixable in
    //          firmware at all).
    Serial.print("[ENC-LINK] own MAC ");
    printAddr(WiFi.macAddress());
    Serial.print(" | ap ");
    Serial.print(WiFi.SSID());
    Serial.print(" | ip ");
    Serial.print(WiFi.localIP());
    Serial.print(" | bssid ");
    printAddr(WiFi.BSSID());
    Serial.print(" | we=ch");
    Serial.print(WiFi.channel());
    Serial.print(" | peer ");
    if (peerSet)
        printMacLn(peerMac);
    else
        Serial.println("<none>");

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

EnclosureLinkStats enclosureLinkStats()
{
    EnclosureLinkStats s;
    s.rxOk = rxOk;
    s.txOk = txOk;
    s.txFail = txFail;
    s.dropLen = dropLen;
    s.dropMagic = dropMagic;
    s.dropSender = dropSender;
    s.dropBusy = dropBusy;
    s.lastDrop = lastDrop;
    s.channel = peerChannel;
    s.lastRxMs = lastRxMs;
    s.lastTxMs = lastTxMs;
    s.hellosRx = hellosRx;
    s.peerLearned = learned;
    s.peerChannelReported = peerDiscChannel;
    return s;
}

void enclosureLinkSend(const EnclosureCommand &cmd)
{
    if (commandQueue)
        xQueueSend(commandQueue, &cmd, 0);
}
