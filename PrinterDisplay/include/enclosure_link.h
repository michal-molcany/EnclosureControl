#ifndef ENCLOSURE_LINK_H
#define ENCLOSURE_LINK_H

#include <Arduino.h>
#include <enclosure_proto.h>

// Live telemetry from EnclosureFanControl. Produced by the background link
// task (core 0) which owns ESP-NOW; consumed by the UI task, which must never
// touch ESP-NOW directly. Same pattern as poller.h.
struct EnclosureSnapshot
{
    uint8_t louvrePct = 0;
    uint8_t fanPct = 0;
    int16_t chamberCx10 = ENC_CHAMBER_INVALID;
    uint8_t state = ENC_STATE_IDLE;
    uint8_t mode = ENC_MODE_AUTO;
    uint8_t seq = 0;
    unsigned long rxMs = 0;
    bool valid = false;
};

// Starts the link task. Call AFTER WiFi is connected (ESP-NOW shares the STA
// channel). peerMac may be all-zeros: receive still works, transmit skipped.
// Returns immediately.
void enclosureLinkStart(const uint8_t *peerMac);

// Copies the latest snapshot. Returns false if the mutex was busy (skip the
// frame and try again next loop iteration).
bool enclosureLinkCopy(EnclosureSnapshot &out);

// True once at least one telemetry packet has been received.
bool enclosureLinkHasData();

// Non-blocking enqueue of a command for the link task. The task stamps
// magic+seq and sends immediately, then repeats it as heartbeat.
// Safe to call before enclosureLinkStart (drops the packet).
void enclosureLinkSend(const EnclosureCommand &cmd);

#endif
