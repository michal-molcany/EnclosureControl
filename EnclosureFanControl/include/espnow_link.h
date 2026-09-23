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

// True once a peer MAC is configured (transmit enabled).
bool espnowLinkHasPeer();

#endif
