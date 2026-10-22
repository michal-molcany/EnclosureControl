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

// PID calibration lifecycle, mirrored from PidCalibration::Phase on the C3.
// The numeric values are part of the wire format: PidCalibration::Phase is
// appended to for the same reason, so a CYD can render a phase it does not
// understand as "running" rather than falling back to idle.
enum EnclosureCalPhase : uint8_t
{
    ENC_CAL_PHASE_NONE = 0,
    ENC_CAL_PHASE_PREHEATING = 1,
    ENC_CAL_PHASE_SETTLING = 2,
    ENC_CAL_PHASE_RELAY = 3,
    ENC_CAL_PHASE_DONE = 4,
    ENC_CAL_PHASE_ABORTED = 5
};

// Why a calibration stopped. The C3 keeps the full sentence locally for its web
// UI; this compact code is what the CYD needs to show a cause without carrying a
// 96-byte string in every telemetry frame.
enum EnclosureCalError : uint8_t
{
    ENC_CAL_ERR_NONE = 0,
    ENC_CAL_ERR_CANCELLED = 1,
    ENC_CAL_ERR_EMERGENCY = 2,
    ENC_CAL_ERR_CHAMBER_HIGH = 3,
    ENC_CAL_ERR_BED_TIMEOUT = 4,
    ENC_CAL_ERR_OCTO_LOST = 5,
    ENC_CAL_ERR_DATA_STALE = 6,
    ENC_CAL_ERR_PRINT_STARTED = 7,
    ENC_CAL_ERR_TIMEOUT = 8,
    ENC_CAL_ERR_BAD_READING = 9,
    ENC_CAL_ERR_OTHER = 10
};

// Max OctoPrint API key length, including the NUL. OctoPrint issues 32-character
// keys, so this is deliberately double that: it costs 33 bytes in a frame that is
// nowhere near the 127-byte ESP-NOW payload limit, and a future longer key must
// not silently truncate into something that authenticates as a different
// credential.
#define ENC_OCTO_KEY_MAX 65

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
    // The OctoPrint API key, for the same reason octoIp is here: one setting,
    // one owner. The C3 can have it changed in its web UI at runtime; without
    // this the CYD would be left polling with the key frozen into its own
    // firmware at build time, and the two boards would disagree about which
    // credentials are current.
    //
    // CREDENTIAL ON THE AIR, DELIBERATELY. This is a cleartext ESP-NOW unicast
    // (peer.encrypt is false), so this field puts the OctoPrint credential on
    // the air whenever it is actually sent - see octoKeyFlags for how rarely
    // that is. It is a real widening of the blast radius of a leak: anyone
    // within radio range, or anyone reading either board's flash, obtains the
    // key. It is a considered trade for "one place to configure the printer",
    // not an oversight. The alternative - carrying the key only over the
    // token-protected HTTP link - reintroduces two sources of truth, which is
    // the failure this whole field exists to prevent.
    //
    // octoKeyFlags says whether octoKey is meaningful. An EMPTY KEY ALONE IS NOT
    // A SIGNAL - see ENC_KEYFLAG_* below, because a naive "empty means no
    // override" reading makes this field unusable on a mostly-silent wire.
    char octoKey[ENC_OCTO_KEY_MAX];
    uint8_t octoKeyFlags;

    // Bed temperature, same 0.1 C encoding and invalid sentinel as chamberCx10.
    // The CYD needs it only for the modal calibration screen, but it is cheap
    // and keeping it here means the same field is available over both
    // transports without a second request.
    int16_t bedCx10;
    // Calibration progress. `calPhase` is EnclosureCalPhase; ETA is app-side,
    // so the wire only carries elapsed time and completed relay cycles.
    uint16_t calElapsedSec;
    uint16_t calCycles;
    uint8_t calPhase;
    uint8_t calBedTargetC;
    uint8_t calSetpointC;
    uint8_t calErrorCode; // EnclosureCalError, meaningful when calPhase == ABORTED
};

// Whether octoKey carries a value, and whether the receiver should act on it.
//
// WHY A FLAGS BYTE AND NOT AN EMPTY-STRING SENTINEL
// -------------------------------------------------
// The key is NOT sent in every frame (a credential is worth keeping off the air
// when nothing has changed), but the CYD still has to be able to fall back to its
// own compiled-in key when the C3's override is cleared. Those two needs
// collide: with "empty means no override", the frames that carry no key - the
// overwhelming majority - would read as "drop your override" and the CYD would
// flip back to its own key once per second.
//
// So there are three states, and only two of them are actionable:
//
//   flags == 0                      NOTHING TO SAY. The receiver keeps whatever
//                                    it already has. This is what the in-between
//                                    frames carry, and it is why a sparse cadence
//                                    is safe at all.
//   ENC_KEYFLAG_PRESENT             octoKey holds a real key; use it.
//   ENC_KEYFLAG_CLEAR               The C3 has no override; drop yours and
//                                    return to your compiled-in default.
//
// PRESENT and CLEAR are mutually exclusive; a receiver must reject both set,
// rather than picking one, because that state has no meaning.
#define ENC_KEYFLAG_PRESENT 0x01
#define ENC_KEYFLAG_CLEAR 0x02

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
    // One-shot calibration action: 0 = none, nonzero = "cancel the active run".
    // The C3 acts only when this value CHANGES, so the 2 s heartbeat repeats it
    // harmlessly. A CYD that reboots sends 0 first, which resets the C3's
    // remembered value; the next confirmed cancel therefore works even after a
    // collision with a pre-reboot action number.
    uint8_t calAction;
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
static_assert(sizeof(EnclosureTelemetry) == 92, "EnclosureTelemetry must stay 92 bytes on the wire");
static_assert(offsetof(EnclosureTelemetry, magic) == 0, "telemetry magic offset");
static_assert(offsetof(EnclosureTelemetry, seq) == 2, "telemetry seq offset");
static_assert(offsetof(EnclosureTelemetry, louvrePct) == 3, "telemetry louvrePct offset");
static_assert(offsetof(EnclosureTelemetry, fanPct) == 4, "telemetry fanPct offset");
static_assert(offsetof(EnclosureTelemetry, chamberCx10) == 6, "telemetry chamberCx10 needs 1 pad byte at offset 5");
static_assert(offsetof(EnclosureTelemetry, state) == 8, "telemetry state offset");
static_assert(offsetof(EnclosureTelemetry, mode) == 9, "telemetry mode offset");
static_assert(offsetof(EnclosureTelemetry, octoIp) == 10, "telemetry octoIp offset");
static_assert(offsetof(EnclosureTelemetry, octoPort) == 14, "telemetry octoPort offset");
static_assert(offsetof(EnclosureTelemetry, octoKey) == 16, "telemetry octoKey offset");
static_assert(offsetof(EnclosureTelemetry, octoKeyFlags) == 81, "telemetry octoKeyFlags offset");
static_assert(offsetof(EnclosureTelemetry, bedCx10) == 82, "telemetry bedCx10 offset");
static_assert(offsetof(EnclosureTelemetry, calElapsedSec) == 84, "telemetry calElapsedSec offset");
static_assert(offsetof(EnclosureTelemetry, calCycles) == 86, "telemetry calCycles offset");
static_assert(offsetof(EnclosureTelemetry, calPhase) == 88, "telemetry calPhase offset");
static_assert(offsetof(EnclosureTelemetry, calBedTargetC) == 89, "telemetry calBedTargetC offset");
static_assert(offsetof(EnclosureTelemetry, calSetpointC) == 90, "telemetry calSetpointC offset");
static_assert(offsetof(EnclosureTelemetry, calErrorCode) == 91, "telemetry calErrorCode offset");
static_assert(sizeof(EnclosureTelemetry) < 127, "ESP-NOW payload limit");

static_assert(sizeof(EnclosureCommand) == 10, "EnclosureCommand must stay 10 bytes on the wire");
static_assert(offsetof(EnclosureCommand, magic) == 0, "command magic offset");
static_assert(offsetof(EnclosureCommand, seq) == 2, "command seq offset");
static_assert(offsetof(EnclosureCommand, mode) == 3, "command mode offset");
static_assert(offsetof(EnclosureCommand, setpoint) == 4, "command setpoint offset");
static_assert(offsetof(EnclosureCommand, louvrePct) == 5, "command louvrePct offset");
static_assert(offsetof(EnclosureCommand, fanPct) == 6, "command fanPct offset");
static_assert(offsetof(EnclosureCommand, material) == 7, "command material offset");
static_assert(offsetof(EnclosureCommand, jobActive) == 8, "command jobActive offset");
static_assert(offsetof(EnclosureCommand, calAction) == 9, "command calAction reuses the old trailing pad byte");

#endif // ENCLOSURE_PROTO_H
