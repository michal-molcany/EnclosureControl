#ifndef ESPNOW_LINK_H
#define ESPNOW_LINK_H

#include <Arduino.h>
#include <enclosure_proto.h>

class TemperatureRegulation;

// Call once after WiFi is connected (ESP-NOW shares the STA channel).
// peerMac may be all-zeros: receive still works, transmit is skipped until set.
void espnowLinkBegin(const uint8_t *peerMac);

// Call from loop(): applies inbound commands, sends telemetry, re-syncs channel.
// chamberValid=false sends ENC_CHAMBER_INVALID.
void espnowLinkPoll(TemperatureRegulation &reg, double chamberTemp, bool chamberValid);

// Re-asserts the ENC_WIFI_CHANNEL pin. Call this from loop() on EVERY iteration,
// before espnowLinkPoll(): the access point re-synchronises the station to its own
// channel on every beacon (~100ms), so a pin re-asserted on a slow timer is
// off-channel when telemetry is transmitted and ESP-NOW still fails. It is
// harmless when ENC_WIFI_CHANNEL is 0 or the pin has been abandoned.
void espnowLinkMaintainChannel(unsigned long now);

// True once a peer MAC is configured (transmit enabled).
bool espnowLinkHasPeer();

// Cumulative link counters, snapshot at call time. Backs the periodic
// "[LINK] ch=..." status line that espnowLinkPoll() prints.
struct EnclosureLinkStats
{
    uint32_t rxOk = 0;    // commands accepted into the inbound slot
    uint32_t txOk = 0;    // ESP-NOW send callback reported success
    uint32_t txFail = 0;  // ESP-NOW send callback reported failure
    uint32_t dropLen = 0;
    uint32_t dropMagic = 0;
    uint32_t dropSender = 0;
    uint32_t dropBusy = 0; // a command was already waiting in the slot
    uint8_t lastDrop = ENC_DROP_NONE;
    int channel = 0;        // channel the peer is registered on
    unsigned long lastRxMs = 0;
    unsigned long lastTxMs = 0;
    uint32_t hellosRx = 0;  // discovery hellos heard from any peer
    bool peerLearned = false;               // peer came from discovery, not const.h
    uint8_t peerChannelReported = 0;        // channel the peer says it is on
};
EnclosureLinkStats espnowLinkStats();

#endif
