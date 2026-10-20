#include "enclosure_link.h"
#include "enclosure_discovery.h"
#include "enclosure_channel.h"
#include "const.h"
#include "enclosure_api_client.h"
#include "poller.h"

#include <WiFi.h>
#include <IPAddress.h>
#include <WiFiClient.h>
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

    // ESP-NOW receive timestamp, kept separate from lastRxMs (which the API
    // fallback also updates). Only this one decides whether ESP-NOW is still the
    // live transport, so a successful API poll can never be mistaken for ESP-NOW
    // coming back and prematurely switch the arbiter off.
    unsigned long lastEspNowRxMs = 0;

    // HTTP API fallback (lib/EnclosureApi). Engages only while ESP-NOW is stale -
    // the channel/AP split no channel pin can fix - and rides the shared IP
    // network instead. ESP-NOW remains primary; see the header comment block.
    IPAddress apiHost;
    bool apiHostKnown = false;
    bool apiResolveFailed = false;
    // False until the first resolve attempt. A separate flag rather than
    // testing apiResolveMs == 0, because millis() legitimately returns 0 just
    // after boot and that must not be mistaken for "never tried".
    bool apiResolveTried = false;
    unsigned long apiResolveMs = 0;
    unsigned long apiPollMs = 0;
    unsigned long apiCmdMs = 0;
    unsigned long lastApiDiagMs = 0;
    bool apiActive = false;
    unsigned long lastApiRxMs = 0;
    // True while an address is cached but every request against it has failed.
    // Distinguishes "we have no idea where the C3 is" from "we know exactly
    // where it is and it is not answering", which need very different retry
    // cadences - see apiResolveDue().
    bool apiHostUnreachable = false;
    // Reused socket for every API request. Held here rather than inside the
    // client calls so the connection is opened once per poll/heartbeat instead
    // of once per HTTPClient, and so the type is a plain WiFiClient - see the
    // TLS note in enclosure_api_client.h.
    WiFiClient apiSock;

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

    // The ESP-NOW link key, copied out of the shared header once.
    const uint8_t ltk[ESP_NOW_KEY_LEN] = ENC_ESP_NOW_LTK;

    // ESP-NOW's key is AES-128-CCM and the driver rejects any other length. This
    // build predates the lmk_len field (ESP-IDF v5), so the length is fixed by
    // the struct rather than carried per-peer.
    //
    // A second array with a DEDUCED size is what makes the check meaningful: if
    // the key header carries the wrong number of bytes, `ltk` would simply be
    // zero-padded to fit and nothing would complain, producing a key that works
    // only by accident. Asserting on the deduced size catches that at build time
    // instead of as a link that will not come up.
    static constexpr uint8_t ltkDeduced[] = ENC_ESP_NOW_LTK;
    static_assert(sizeof(ltkDeduced) == ESP_NOW_KEY_LEN,
                  "ENC_ESP_NOW_LTK must contain exactly ESP_NOW_KEY_LEN bytes");

    // Registers a unicast peer at a channel, ENCRYPTED.
    //
    // Used both for the compiled-in address and for one learned from a discovery
    // hello.
    //
    // Encryption is not optional decoration here: telemetry carries the OctoPrint
    // API key, and ESP-NOW frames never leave the radio, so an unencrypted link
    // puts a live printer credential on the air for anything in range to read.
    // WPA2 does not help - these frames are not carried by the access point at
    // all.
    //
    // It also makes peer hijacking much less useful: an attacker who answers a
    // discovery hello and takes the peer slot receives ciphertext they have no
    // key for, instead of the key itself.
    //
    // It does NOT authenticate what we RECEIVE - see the note in
    // include/espnow_key.h.
    bool addOrUpdatePeer(const uint8_t *mac, int channel)
    {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, mac, 6);
        peer.channel = (uint8_t)channel;
        peer.encrypt = true;
        memcpy(peer.lmk, ltk, sizeof(ltk));
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
    //
    // DELIBERATELY NOT ENCRYPTED. ESP-NOW encryption is per-unicast-peer and
    // does not work against a broadcast destination, so an encrypted broadcast
    // peer would simply stop the hellos going out.
    //
    // The cost is that peer discovery stays unauthenticated: anyone can answer a
    // hello and take the peer slot. What that buys an attacker changes, though -
    // they are added as an ENCRYPTED peer, so everything we send to them is
    // ciphertext they hold no key for.
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

        // The OctoPrint address the C3 is using, taken from every frame.
        //
        // Applied OUTSIDE the snapshot mutex and before the sender/peer checks:
        // it is the one field whose delivery matters more than the telemetry
        // around it, it costs one comparison, and pollerSetEndpoint() only
        // publishes a value - it cannot block or fail. An all-zero address means
        // "the C3 has no override", which makes this board fall back to its own
        // compiled-in default.
        pollerSetEndpoint(IPAddress(tele->octoIp[0], tele->octoIp[1],
                                    tele->octoIp[2], tele->octoIp[3]),
                          tele->octoPort);
        // The API key, same call the HTTP fallback makes (see
        // serviceApiFallback) so both transports hand the key over through one
        // path and cannot drift in how they interpret a frame.
        //
        // Applied before the peer/sender checks for the same reason as the
        // address above: it only publishes a value, it cannot block or fail,
        // and refusing it because the sender was not the paired peer would mean
        // a legitimate configuration never arrives while in bootstrap mode.
        pollerSetApiKey(tele->octoKey, tele->octoKeyFlags);
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
        shared.fromApi = false;
        xSemaphoreGive(snapshotMutex);
        lastRxMs = shared.rxMs;
        lastEspNowRxMs = shared.rxMs;
        ++rxOk;
        hasData = true;
    }

    // Decides whether it is time to re-resolve the C3's address.
    //
    // This gate is the whole reason the fallback is not a network busy-loop.
    // resolve() costs up to 1.5 s of blocking mDNS query, and both failure paths
    // below (resolve failed, request failed) invalidate apiHostKnown - which is
    // also the "resolve now" condition. Testing `!apiHostKnown` alone therefore
    // retried on every single link tick, i.e. an mDNS query every ~50 ms for as
    // long as ESP-NOW stayed down: the exact situation the fallback exists for.
    //
    // So the retry rate depends on WHY we last lost the host:
    //   no host at all      -> retry on the normal resolve cadence, since the
    //                          cause is most likely a C3 that is simply not up
    //                          yet and will answer as soon as it is.
    //   host known, refusing-> retry only on the slow unreachable backoff. The
    //                          address is almost certainly still right (a DHCP
    //                          move would be a separate, rarer event), so
    //                          re-resolving quickly just burns mDNS traffic and
    //                          keeps the task blocked.
    bool apiResolveDue(unsigned long now)
    {
        // Never attempted: resolve at once. Without this the very first attempt
        // is gated on `now - apiResolveMs`, and apiResolveMs starts at 0, so the
        // first resolve is deferred a full ENC_API_RESOLVE_MS (30 s) after boot.
        // That is exactly when the fallback is most needed - it is the window in
        // which the C3 is still coming up on the network - so the first attempt
        // must not wait.
        if (!apiResolveTried)
            return true;
        const unsigned long due = apiHostUnreachable ? ENC_API_UNREACHABLE_RETRY_MS
                                                     : ENC_API_RESOLVE_MS;
        return (now - apiResolveMs) >= due;
    }

    // HTTP API fallback. Called only while ESP-NOW telemetry is stale (i.e. the
    // channel/AP split). Pulls telemetry from the C3 and pushes commands back over
    // the IP network that both boards share. Feeds the SAME `shared` snapshot the
    // UI consumes, so the chamber view and both header icons behave identically
    // whichever transport is live. Best-effort throughout: any failure just drops
    // back to re-resolving and leaves the last snapshot in place.
    void serviceApiFallback(unsigned long now, bool cmdFresh)
    {
        // (Re)resolve the C3 address initially, on a slow cadence, and after any
        // request fails (a DHCP lease can move it).
        if (!apiHostKnown ? apiResolveDue(now) : false)
        {
            apiResolveMs = now;
            apiResolveTried = true;
            if (EnclosureApi::resolve(apiHost, ENC_API_MDNS_NAME, ENC_API_STATIC_IP))
            {
                apiHostUnreachable = false;
                if (!apiHostKnown)
                {
                    apiHostKnown = true;
                    apiResolveFailed = false;
                    Serial.print("[ENC-LINK] api: resolved ");
                    Serial.println(apiHost.toString());
                }
            }
            else
            {
                if (!apiResolveFailed)
                {
                    apiResolveFailed = true;
                    Serial.println("[ENC-LINK] api: resolve FAILED (mDNS + static fallback)");
                }
                apiHostKnown = false;
                apiActive = false;
                return;
            }
        }

        // Refresh a known-but-unreachable address on the slow cadence only.
        if (apiHostKnown && apiHostUnreachable && apiResolveDue(now))
        {
            apiResolveMs = now;
            if (EnclosureApi::resolve(apiHost, ENC_API_MDNS_NAME, ENC_API_STATIC_IP))
            {
                apiHostUnreachable = false;
                Serial.print("[ENC-LINK] api: re-resolved ");
                Serial.println(apiHost.toString());
            }
        }

        if (!apiHostKnown)
        {
            apiActive = false;
            return;
        }

        // Telemetry, one request per ENC_API_POLL_MS. getTelemetry()/postCommand()
        // can block up to ~1.2 s; this task is pure housekeeping, so that is fine.
        if (now - apiPollMs >= ENC_API_POLL_MS)
        {
            apiPollMs = now;
            EnclosureTelemetry t = {};
            if (EnclosureApi::getTelemetry(apiHost, ENC_API_PORT, ENC_API_TOKEN, apiSock, t))
            {
                // Same endpoint hand-off as the ESP-NOW path, so a change made on
                // the web UI still reaches this board while the radio link is
                // down and the HTTP fallback is carrying telemetry.
                pollerSetEndpoint(IPAddress(t.octoIp[0], t.octoIp[1], t.octoIp[2],
                                            t.octoIp[3]),
                                  t.octoPort);
                pollerSetApiKey(t.octoKey, t.octoKeyFlags);
                if (xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(5)) == pdTRUE)
                {
                    shared.louvrePct = t.louvrePct;
                    shared.fanPct = t.fanPct;
                    shared.chamberCx10 = t.chamberCx10;
                    shared.state = t.state;
                    shared.mode = t.mode;
                    shared.seq = t.seq;
                    shared.rxMs = millis();
                    shared.valid = true;
                    shared.fromApi = true;
                    xSemaphoreGive(snapshotMutex);
                    lastRxMs = shared.rxMs;
                    hasData = true;
                    apiActive = true;
                    apiHostUnreachable = false;
                    lastApiRxMs = shared.rxMs;
                }
            }
            else
            {
                // Address is probably still correct; stop hammering it and let
                // the slow backoff decide when to re-resolve.
                apiHostUnreachable = true;
                apiActive = false;
            }
        }

        // Command: push on change OR every heartbeat. The heartbeat matters: the
        // C3's remoteLinkAlive() lapses after ENC_LINK_LOSS_TIMEOUT_MS without a
        // command, so a steady POST keeps it treating the CYD as primary and stops
        // it falling back to its own OctoPrint material.
        //
        // cmdFresh honours a UI change immediately, exactly as the ESP-NOW path
        // does. Without it a manual-mode tap took up to a full heartbeat (2 s) to
        // reach the C3, which reads as the display ignoring the button - the
        // fallback must not be slower to react than the transport it replaces.
        if (hasCmd && (cmdFresh || (now - apiCmdMs >= ENC_CMD_HEARTBEAT_INTERVAL_MS)))
        {
            apiCmdMs = now;
            if (!EnclosureApi::postCommand(apiHost, ENC_API_PORT, ENC_API_TOKEN, apiSock, lastCmd))
                apiHostUnreachable = true;
        }
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

            // ESP-NOW is primary. Only while it has been quiet for a full stale
            // window (the channel/AP split signature) does the HTTP API fallback
            // take over. Based on lastEspNowRxMs, NOT lastRxMs, so a good API poll
            // can never masquerade as ESP-NOW returning.
            const bool espNowFresh = (now - lastEspNowRxMs < ENC_LINK_STALE_MS);

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

            // ESP-NOW commands only while the radio link is the live transport;
            // once the API carries the link, sending into a channel split is
            // futile, so the fallback path owns command duty.
            if (peerSet && hasCmd && espNowFresh)
            {
                if (fresh || (now - lastHeartbeatMs >= ENC_CMD_HEARTBEAT_INTERVAL_MS))
                {
                    lastHeartbeatMs = now;
                    sendCmd(lastCmd);
                }
            }

            // HTTP API fallback: only while ESP-NOW telemetry is stale. `fresh`
            // is the change flag drained above, so a UI change is POSTed at once.
            if (!espNowFresh)
                serviceApiFallback(now, fresh);
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
            // Separate, slower line only while the fallback is (or should be)
            // running, so the transport in use is unambiguous on the log.
            if (!espNowFresh && (now - lastApiDiagMs >= ENC_DIAG_INTERVAL_MS))
            {
                lastApiDiagMs = now;
                Serial.print("[ENC-LINK] api ");
                Serial.print(apiActive ? "UP" : "down");
                Serial.print(" host=");
                Serial.print(apiHostKnown ? apiHost.toString() : String("?"));
                Serial.print(" age=");
                // Recompute "now" rather than reusing the loop's captured value: the API
                // call above blocks for up to ~1.2 s, so `now` predates
                // lastApiRxMs and the subtraction would wrap to a nonsense
                // ~4.29e9 ms.
                Serial.print(apiActive ? (millis() - lastApiRxMs) : 0);
                Serial.print("ms");
                // Name the failure instead of printing a bare "down". "down" is
                // indistinguishable from a dead socket, a rejected token, or a
                // malformed body - and those three need completely different
                // fixes. A 401 in particular means only that the token differs
                // between the two firmware builds.
                if (!apiActive)
                {
                    Serial.print(" why=");
                    Serial.print(EnclosureApi::failureText(EnclosureApi::lastFailure()));
                    const int st = EnclosureApi::lastHttpStatus();
                    if (st)
                    {
                        Serial.print(" http=");
                        Serial.print(st);
                    }
                }
                Serial.println();
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

    // Encryption state, on its own line and NOT behind SERIAL_DEBUG.
    //
    // This is the first thing to check when the link does not come up, because a
    // mismatched LTK on the two boards produces a link that is dead for exactly
    // the same reason as a channel split, a wrong peer MAC, or an absent peer -
    // and nothing anywhere reports an error. Both boards print this; if the two
    // lines disagree about anything, the key is the problem.
    //
    // The key itself is never printed. Only whether one is configured.
    Serial.print(F("[ENC-LINK] espnow encryption ON (16-byte LTK, peer.encrypt=true)"));
#if ENC_WIFI_CHANNEL != 0
    Serial.print(" | channel pin ");
#else
    Serial.print(" | no channel pin");
#endif
    Serial.println();

    snapshotMutex = xSemaphoreCreateMutex();
    commandQueue = xQueueCreate(8, sizeof(EnclosureCommand));
    // Larger than a bare ESP-NOW task needs: the HTTP API fallback uses
    // HTTPClient + ArduinoJson on this same task, which needs headroom beyond 8 KB.
    xTaskCreatePinnedToCore(linkTask, "encLink", 12288, nullptr, 1, nullptr, 0);
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
    s.apiActive = apiActive;
    s.lastApiRxMs = lastApiRxMs;
    s.apiHost = apiHost;
    s.apiHostKnown = apiHostKnown;
    return s;
}

void enclosureLinkSend(const EnclosureCommand &cmd)
{
    if (commandQueue)
        xQueueSend(commandQueue, &cmd, 0);
}
