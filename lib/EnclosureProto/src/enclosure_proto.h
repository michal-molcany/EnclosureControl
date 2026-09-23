// Shared ESP-NOW protocol: PrinterDisplay (CYD) <-> EnclosureFanControl (C3).
//
// Transport: ESP-NOW, fire-and-forget. Reliability comes from repetition:
//   - C3 broadcasts telemetry every TELEMETRY_INTERVAL_MS.
//   - CYD sends a command on every UI change plus a CMD_HEARTBEAT_INTERVAL_MS
//     heartbeat so a lost packet self-heals within seconds.
// Failsafe: if the C3 hears nothing for LINK_LOSS_TIMEOUT_MS while in manual
// mode it falls back to automatic material control.
// Pairing: static peer MACs (see ENC_PEER_MAC in each project's const.h).
// Both boards must sit on the same WiFi channel (same AP); each side
// re-syncs its peer channel to WiFi.channel() in its link task.

#ifndef ENCLOSURE_PROTO_H
#define ENCLOSURE_PROTO_H

#include <stdint.h>

#define ENC_PROTO_MAGIC 0x454E // "EN"

// Regulation state reported by the C3.
enum EnclosureState : uint8_t
{
    ENC_STATE_IDLE = 0,
    ENC_STATE_PLA = 1,
    ENC_STATE_PETG = 2,
    ENC_STATE_ASA = 3
};

// Who controls fan + louvre on the C3.
enum EnclosureMode : uint8_t
{
    ENC_MODE_AUTO = 0,
    ENC_MODE_MANUAL = 1
};

// C3 -> CYD, sent every TELEMETRY_INTERVAL_MS.
struct EnclosureTelemetry
{
    uint16_t magic;      // ENC_PROTO_MAGIC
    uint8_t seq;         // wraps, for loss detection
    uint8_t louvrePct;   // 0-100
    uint8_t fanPct;      // 0-100
    int16_t chamberCx10; // chamber temp in 0.1 C, INT16_MIN when invalid
    uint8_t state;       // EnclosureState
    uint8_t mode;        // EnclosureMode
};

#define ENC_CHAMBER_INVALID INT16_MIN

// CYD -> C3, sent on every UI change + heartbeat.
struct EnclosureCommand
{
    uint16_t magic;    // ENC_PROTO_MAGIC
    uint8_t seq;       // wraps
    uint8_t mode;      // EnclosureMode
    uint8_t setpoint;  // desired chamber C, used in AUTO (20-60)
    uint8_t louvrePct; // 0-100, used in MANUAL
    uint8_t fanPct;    // 0-100, used in MANUAL
};

#define ENC_TELEMETRY_INTERVAL_MS 1000UL
#define ENC_CMD_HEARTBEAT_INTERVAL_MS 2000UL
#define ENC_LINK_LOSS_TIMEOUT_MS 10000UL
#define ENC_CHANNEL_RESYNC_MS 5000UL

#endif // ENCLOSURE_PROTO_H
