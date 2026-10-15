#include "espnow_link.h"
#include "enclosure_discovery.h"
#include "enclosure_channel.h"
#include "octo_endpoint.h"
#include "temperature_regulation.h"
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
    bool linkReady = false;

    SemaphoreHandle_t rxMutex = nullptr;
    EnclosureCommand rxSlot = {};
    volatile bool rxFlag = false;

    uint8_t teleSeq = 0;
    unsigned long lastTeleMs = 0;
    unsigned long lastResyncMs = 0;
    unsigned long lastPeerResyncMs = 0;
    unsigned long lastDiagMs = 0;
    unsigned long lastHelloMs = 0;
    unsigned long lastDiscReportMs = 0;
    uint32_t txOk = 0;
    uint32_t txFail = 0;
    uint32_t rxOk = 0;
    uint32_t dropLen = 0;
    uint32_t dropMagic = 0;
    uint32_t dropSender = 0;
    uint32_t dropBusy = 0;
    uint8_t lastDrop = ENC_DROP_NONE;
    unsigned long lastRxMs = 0;

    // Discovery state.
    uint32_t hellosRx = 0;
    uint8_t peerDiscChannel = 0;
    uint8_t pendingHelloMac[6] = {0};
    volatile bool pendingHello = false;

    bool macEquals(const uint8_t *a, const uint8_t *b)
    {
        return memcmp(a, b, 6) == 0;
    }

#if SERIAL_DEBUG
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
#endif

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

    bool isZeroMac(const uint8_t *mac)
    {
        for (int i = 0; i < 6; ++i)
            if (mac[i] != 0)
                return false;
        return true;
    }

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

    void printMac(const uint8_t *mac)
    {
        for (int i = 0; i < 6; ++i)
        {
            if (i)
                Serial.print(':');
            Serial.printf("%02X", mac[i]);
        }
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
        Serial.print("[LINK] discovery: peer learned ");
        printMacLn(activeMac);
        if (theirChannel != (uint8_t)WiFi.channel())
        {
            Serial.print("[LINK] WARNING peer is on channel ");
            Serial.print(theirChannel);
            Serial.print(", we are on ");
            Serial.println(WiFi.channel());
        }
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
        // they are dispatched before the command checks rather than counted as
        // malformed commands.
        uint8_t role = 0;
        uint8_t theirChannel = 0;
        if (enclosureDiscoveryParse(data, len, ENC_ROLE_C3, &role, &theirChannel))
        {
            ++hellosRx;
            peerDiscChannel = theirChannel;
            // Handled on the next poll rather than here: this runs in the WiFi
            // task and must not call into esp_now peer management.
            memcpy(pendingHelloMac, mac, 6);
            pendingHello = true;
            return;
        }

        if (len != (int)sizeof(EnclosureCommand))
        {
            noteDrop(ENC_DROP_LEN);
            return;
        }
        const EnclosureCommand *cmd = (const EnclosureCommand *)data;
        if (cmd->magic != ENC_PROTO_MAGIC)
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
        if (rxMutex && xSemaphoreTake(rxMutex, 0) == pdTRUE)
        {
            memcpy(&rxSlot, data, sizeof(rxSlot));
            rxFlag = true;
            xSemaphoreGive(rxMutex);
            lastRxMs = millis();
            ++rxOk;
        }
        else
        {
            noteDrop(ENC_DROP_BUSY);
        }
    }
} // namespace

void espnowLinkBegin(const uint8_t *peerMacIn)
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
    //   ap    - printed because this board's SSID comes from NVS via WiFiManager
    //          and is otherwise never visible, so an SSID mismatch is invisible.
    Serial.print("[LINK] own MAC ");
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

    if (esp_now_init() != ESP_OK)
    {
        Serial.println("[LINK] esp_now_init FAILED");
        return;
    }
    esp_now_register_send_cb(onSent);
    esp_now_register_recv_cb(onRecv);
    rxMutex = xSemaphoreCreateMutex();
    addBroadcastPeer();

    // Single owner for the channel: this function applies the pin and
    // espnowLinkPoll keeps asserting it, so main() does not have to know.
    channelPin.begin(ENC_WIFI_CHANNEL);
    if (ENC_WIFI_CHANNEL != 0)
    {
        Serial.print("[LINK] channel pin ");
        Serial.print(enclosurePinChannel(ENC_WIFI_CHANNEL) ? "requested " : "REFUSED ");
        Serial.print(ENC_WIFI_CHANNEL);
        Serial.print(" -> actual ");
        Serial.println(WiFi.channel());
    }
    else
    {
        Serial.print("[LINK] joined on channel ");
        Serial.println(WiFi.channel());
    }

    if (peerSet)
    {
        int ch = WiFi.channel();
        if (addOrUpdatePeer(activeMac, ch))
        {
#if SERIAL_DEBUG
            Serial.print("[LINK] peer added on channel ");
            Serial.println(ch);
#endif
        }
        else
        {
            Serial.println("[LINK] peer add FAILED");
        }
    }
    else
    {
        Serial.println("[LINK] no ENC_PEER_MAC - relying on discovery");
    }
    linkReady = true;
}

void espnowLinkMaintainChannel(unsigned long now)
{
    // The AP re-synchronises the station to its own channel on every beacon, so
    // this has to run at loop rate, not on a timer. It is a no-op when
    // ENC_WIFI_CHANNEL is 0 or the pin has already been abandoned.
    channelPin.maintain(now, WiFi.channel(), "[LINK]");
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

    // Adopt a peer discovered on the air. Done here, not in the callback,
    // because it calls into esp_now peer management.
    if (pendingHello)
    {
        pendingHello = false;
        adoptLearnedPeer(pendingHelloMac, peerDiscChannel);
    }

    // Broadcast our own hello. This is what makes a wrong ENC_PEER_MAC
    // self-correcting, and "no hellos heard" is the positive signal that the
    // boards are on different channels or APs.
    if (now - lastHelloMs >= ENC_DISCOVERY_INTERVAL_MS)
    {
        lastHelloMs = now;
        EnclosureDiscovery hello = {};
        hello.magic = ENC_DISCOVERY_MAGIC;
        hello.role = ENC_ROLE_C3;
        hello.channel = (uint8_t)WiFi.channel();
        esp_now_send(broadcastMac, (const uint8_t *)&hello, sizeof(hello));
    }

#if SERIAL_DEBUG
    // Discovery summary, on its own slower cadence so it stays readable next
    // to the link status line.
    if (now - lastDiscReportMs >= ENC_DISCOVERY_REPORT_MS)
    {
        lastDiscReportMs = now;
        Serial.print("[LINK] discovery hellosRx=");
        Serial.print(hellosRx);
        Serial.print(" we=ch");
        Serial.print(WiFi.channel());
        if (hellosRx == 0)
        {
            // The single most useful line this firmware can print. Neither
            // board can hear the other on a different channel, so the channel
            // is the thing to check first - and the two boards have to be
            // compared by hand, since neither can detect a mismatch itself.
            Serial.println(" NO PEER HEARD");
            Serial.print("[LINK] ACTION both boards must be on the same AP AND channel. We are on channel ");
            Serial.print(WiFi.channel());
            Serial.println(" - compare with the CYD's 'joined on channel' line.");
        }
        else
        {
            Serial.print(" peer=ch");
            Serial.print(peerDiscChannel);
            Serial.print(" mac=");
            printMacLn(activeMac);
        }
    }
#endif

    // NOTE: the channel pin is deliberately NOT maintained here. This function is
    // only reached once per second, and the AP pulls the station back onto its own
    // channel roughly every 100ms, so a pin re-asserted at 1Hz cannot hold.
    // espnowLinkMaintainChannel() is called from loop() instead.

    // ESP-NOW only works on the STA channel; follow AP channel changes.
    if (peerSet && (now - lastPeerResyncMs >= ENC_CHANNEL_RESYNC_MS))
    {
        lastPeerResyncMs = now;
        int ch = WiFi.channel();
        if (ch != peerChannel)
        {
            if (addOrUpdatePeer(activeMac, ch))
            {
#if SERIAL_DEBUG
                Serial.print("[LINK] peer channel resync -> ");
                Serial.println(ch);
#endif
            }
            else
            {
                Serial.print("[LINK] peer channel resync FAILED on channel ");
                Serial.println(ch);
            }
        }
#if SERIAL_DEBUG
        // Printed on every tick, not only on change: a peer that is stuck on a
        // different channel is otherwise invisible, and the two boards would
        // have to be compared by hand to notice.
        else
        {
            Serial.print("[LINK] ch ");
            Serial.print(ch);
            Serial.print(" peer registered on ");
            Serial.println(peerChannel);
        }
#endif
    }

    // One line per ENC_DIAG_INTERVAL_MS. Reading it:
    //   rx=0, drop=0, last=-
    //            -> nothing is arriving at all: wrong peer MAC, or the peer is
    //               on another AP or channel
    //   last=sender -> frames arrive but the source MAC is not our peer
    //   last=busy   -> accepted but dropped because a command was already
    //                  waiting in the slot
    //   tx=0/N     -> our telemetry is failing outright
#if SERIAL_DEBUG
    if (peerSet && (now - lastDiagMs >= ENC_DIAG_INTERVAL_MS))
    {
        lastDiagMs = now;
        const uint32_t drops = dropLen + dropMagic + dropSender + dropBusy;
        Serial.print("[LINK] ch=");
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
#endif

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
        // The address we are polling, so the CYD can use the same one. Sent
        // every frame; the CYD stores it and repoints its own poller. All-zero
        // means "no override here", which tells the CYD to fall back to its own
        // compiled-in default.
        const IPAddress octo = octoEndpointIp();
        tele.octoIp[0] = octo[0];
        tele.octoIp[1] = octo[1];
        tele.octoIp[2] = octo[2];
        tele.octoIp[3] = octo[3];
        tele.octoPort = octoEndpointPort();
        const esp_err_t err = esp_now_send(activeMac, (const uint8_t *)&tele, sizeof(tele));
        if (err != ESP_OK)
        {
            ++txFail;
#if SERIAL_DEBUG
            // The bare ESP-NOW codes are opaque on their own; name the two that
            // actually show up (peer unknown, and buffer exhaustion).
            Serial.print("[LINK] telemetry send failed: ");
            Serial.println(err == ESP_ERR_ESPNOW_NOT_FOUND ? "peer not registered" : esp_err_to_name(err));
#endif
        }
    }
}

bool espnowLinkHasPeer()
{
    return peerSet;
}

EnclosureLinkStats espnowLinkStats()
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
    s.lastTxMs = lastTeleMs;
    s.hellosRx = hellosRx;
    s.peerLearned = learned;
    s.peerChannelReported = peerDiscChannel;
    return s;
}
