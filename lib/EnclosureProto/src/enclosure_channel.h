// Wi-Fi channel pinning, shared by both boards.
//
// Why this exists
// ---------------
// ESP-NOW frames never leave the radio, so two boards only hear each other when
// they share a channel. "The same SSID" does not guarantee that: an access
// point with roaming, band steering, or per-client channel assignment will
// happily associate two boards on one SSID to different channels, and the link
// then fails in the most opaque way possible - every send reports failure and
// not a single frame ever arrives, with no error anywhere.
//
// The fix is normally network-side (one AP, one fixed channel). This is the
// firmware-side lever for the cases where the AP will tolerate it, and it is
// off by default because a mismatched pin can stop the board associating at
// all - which looks exactly like a dead Wi-Fi connection.
#ifndef ENCLOSURE_CHANNEL_H
#define ENCLOSURE_CHANNEL_H

#include <Arduino.h>
#include <esp_wifi.h>

// Forces the station onto a specific channel. Returns true if the driver
// accepted the request.
//
// This is best effort, not a guarantee: a station re-synchronises to whatever
// channel the AP beacons on, so a board whose AP is genuinely on another
// channel will drop off the network rather than stay pinned. Always log the
// channel that actually took effect, not the one requested.
inline bool enclosurePinChannel(uint8_t channel)
{
    if (channel == 0)
        return true; // 0 = follow the AP
    if (channel > 14)
        return false;
    return esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) == ESP_OK;
}

// True when the two boards cannot be talking to each other right now. Used only
// for the log line: neither board can hear the other on a different channel, so
// it cannot detect this itself.
inline bool enclosureChannelsDisagree(int own, int theirs)
{
    return own > 0 && theirs > 0 && own != theirs;
}

// How long to keep re-asserting a pin before giving up and letting the AP have
// its way. Without this, pinning a channel the access point actively fights can
// leave a board unable to hear beacons, which drops it off the network
// entirely and - on a board reachable only over the air - makes it
// unrecoverable without a USB flash. Bounding the fight means the worst case is
// a temporarily flaky link instead of an unreachable device.
#define ENC_CHANNEL_PIN_GIVEUP_MS 20000UL

// Tracks whether a channel pin is still worth fighting for. Both boards embed
// one of these; see enclosureChannelMaintain().
class EnclosureChannelPin
{
public:
    void begin(uint8_t requested)
    {
        requested_ = requested;
        fightingSinceMs_ = 0;
        abandoned_ = false;
        reported_ = false;
    }

    // Called on a slow tick. Returns true if a pin was (re-)asserted this call.
    // Deliberately independent of link/peer state: a board with no peer still
    // needs its pin maintained, which is exactly the situation a pin is meant
    // to rescue.
    bool maintain(unsigned long now, int currentChannel, const char *tag)
    {
        if (requested_ == 0 || abandoned_)
            return false;
        if (currentChannel == (int)requested_)
        {
            fightingSinceMs_ = 0;
            return false;
        }
        if (fightingSinceMs_ == 0)
            fightingSinceMs_ = now;

        if (now - fightingSinceMs_ >= ENC_CHANNEL_PIN_GIVEUP_MS)
        {
            abandoned_ = true;
            Serial.print(tag);
            Serial.print(": AP refuses pinned channel ");
            Serial.print(requested_);
            Serial.print(" (we are on ");
            Serial.print(currentChannel);
            Serial.println(") - abandoning the pin and following the AP");
            return false;
        }
        const bool ok = enclosurePinChannel(requested_);
        if (!reported_)
        {
            reported_ = true;
            Serial.print(tag);
            Serial.print(": AP moved us off pinned channel ");
            Serial.print(requested_);
            Serial.print(", re-pinning for up to ");
            Serial.print(ENC_CHANNEL_PIN_GIVEUP_MS / 1000);
            Serial.print("s (");
            Serial.print(ok ? "accepted" : "REFUSED");
            Serial.println(")");
        }
        return ok;
    }

    bool abandoned() const { return abandoned_; }
    uint8_t requested() const { return requested_; }

private:
    uint8_t requested_ = 0;
    unsigned long fightingSinceMs_ = 0;
    bool abandoned_ = false;
    bool reported_ = false;
};

#endif // ENCLOSURE_CHANNEL_H
