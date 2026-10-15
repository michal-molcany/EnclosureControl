// Shared ESP-NOW protocol: PrinterDisplay (CYD) <-> EnclosureFanControl (C3).
//
// Transport: ESP-NOW, fire-and-forget peer unicasts. Reliability comes from repetition:
//   - C3 sends telemetry to CYD every TELEMETRY_INTERVAL_MS.
//   - CYD sends a command on every UI change plus a CMD_HEARTBEAT_INTERVAL_MS
//     heartbeat so a lost packet self-heals within seconds.
// Failsafe: after LINK_LOSS_TIMEOUT_MS without CYD commands, manual control
// falls back to automatic regulation and CYD setpoints fall back to C3 material targets.
// Pairing: static peer MACs (see ENC_PEER_MAC in each project's const.h).
// Both boards must sit on the same WiFi channel (same AP); each side
// re-syncs its peer channel to WiFi.channel() in its link task.

#ifndef ENCLOSURE_PROTO_H
#define ENCLOSURE_PROTO_H

#include <stddef.h>
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

enum EnclosureMaterial : uint8_t
{
    ENC_MATERIAL_PLA = 0,
    ENC_MATERIAL_PETG = 1,
    ENC_MATERIAL_ASA = 2,
    ENC_MATERIAL_ABS = 3,
    ENC_MATERIAL_TPU = 4,
    ENC_MATERIAL_NYLON = 5,
    ENC_MATERIAL_HIGH_TEMP = 6,
    ENC_MATERIAL_UNKNOWN = 7
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
    // The OctoPrint address the C3 is itself polling, so the CYD uses the same
    // one instead of holding a second, independently-editable copy.
    //
    // Sent in EVERY frame, not only when it changes. There is no ack and no C3
    // to CYD back channel, so "repeat until the far end has it" is the whole
    // reliability story: a CYD that boots late, or was powered off while the
    // address changed, converges within one frame (~1 s) with no state to keep
    // on either board.
    //
    // ALL-ZERO octoIp means "the C3 has no runtime override; use your own
    // compiled-in default". That sentinel is what lets a cleared override fall
    // back to the other board's value instead of the two boards silently
    // disagreeing.
    //
    // Bytes rather than an IPAddress: a 4-byte array keeps the struct at 16
    // bytes on the wire with no alignment padding.
    uint8_t octoIp[4];
    uint16_t octoPort;
};

#define ENC_CHAMBER_INVALID INT16_MIN

// CYD -> C3, sent on every UI change + heartbeat.
struct EnclosureCommand
{
    uint16_t magic;      // ENC_PROTO_MAGIC
    uint8_t seq;         // wraps
    uint8_t mode;        // EnclosureMode
    uint8_t setpoint;    // desired chamber C, used in AUTO (20-60)
    uint8_t louvrePct;   // 0-100, used in MANUAL
    uint8_t fanPct;      // 0-100, used in MANUAL
    uint8_t material;
    uint8_t jobActive;
};

#define ENC_TELEMETRY_INTERVAL_MS 1000UL
#define ENC_CMD_HEARTBEAT_INTERVAL_MS 2000UL
#define ENC_LINK_LOSS_TIMEOUT_MS 10000UL
#define ENC_CHANNEL_RESYNC_MS 5000UL
// How often each board prints its one-line link status. A silent link is the
// hardest failure mode to debug, so both boards report on a timer rather than
// only on state change.
#define ENC_DIAG_INTERVAL_MS 5000UL

// Why an inbound packet was discarded. Both boards use the same set so a
// capture from either side reads the same way.
enum EnclosureRxDrop : uint8_t
{
    ENC_DROP_NONE = 0,
    ENC_DROP_LEN,    // length != sizeof(the expected struct)
    ENC_DROP_MAGIC,  // magic != ENC_PROTO_MAGIC
    ENC_DROP_SENDER, // source MAC != the configured peer
    ENC_DROP_BUSY,   // snapshot mutex not acquired in time
};

// Both packet types are sent as the raw in-memory struct using sizeof(), so
// alignment padding is part of the wire format. These asserts turn a silent
// mismatch - which shows up only as "the link is dead" - into a build error.
// ESP32 (CYD) and ESP32-C3 (fan control) are both 32-bit little-endian with
// 2-byte alignment on uint16_t/int16_t, so the two layouts must agree.
static_assert(sizeof(EnclosureTelemetry) == 16, "EnclosureTelemetry must stay 16 bytes on the wire");
static_assert(offsetof(EnclosureTelemetry, magic) == 0, "telemetry magic offset");
static_assert(offsetof(EnclosureTelemetry, seq) == 2, "telemetry seq offset");
static_assert(offsetof(EnclosureTelemetry, louvrePct) == 3, "telemetry louvrePct offset");
static_assert(offsetof(EnclosureTelemetry, fanPct) == 4, "telemetry fanPct offset");
static_assert(offsetof(EnclosureTelemetry, chamberCx10) == 6, "telemetry chamberCx10 needs 1 pad byte at offset 5");
static_assert(offsetof(EnclosureTelemetry, state) == 8, "telemetry state offset");
static_assert(offsetof(EnclosureTelemetry, mode) == 9, "telemetry mode offset");
static_assert(offsetof(EnclosureTelemetry, octoIp) == 10, "telemetry octoIp offset");
static_assert(offsetof(EnclosureTelemetry, octoPort) == 14, "telemetry octoPort offset");

static_assert(sizeof(EnclosureCommand) == 10, "EnclosureCommand must stay 10 bytes on the wire");
static_assert(offsetof(EnclosureCommand, magic) == 0, "command magic offset");
static_assert(offsetof(EnclosureCommand, seq) == 2, "command seq offset");
static_assert(offsetof(EnclosureCommand, mode) == 3, "command mode offset");
static_assert(offsetof(EnclosureCommand, setpoint) == 4, "command setpoint offset");
static_assert(offsetof(EnclosureCommand, louvrePct) == 5, "command louvrePct offset");
static_assert(offsetof(EnclosureCommand, fanPct) == 6, "command fanPct offset");
static_assert(offsetof(EnclosureCommand, material) == 7, "command material offset");
static_assert(offsetof(EnclosureCommand, jobActive) == 8, "command jobActive offset");

#endif // ENCLOSURE_PROTO_H
