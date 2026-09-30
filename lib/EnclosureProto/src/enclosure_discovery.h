// Shared ESP-NOW peer discovery: PrinterDisplay (CYD) <-> EnclosureFanControl (C3).
//
// Why this exists
// ---------------
// Both boards used to require the other one's Wi-Fi MAC to be hard-coded in
// ENC_PEER_MAC. A wrong value there is invisible on the hardware: the C3 only
// shows its pairing screen for an all-zero MAC, so a merely *incorrect* one
// looks exactly like a healthy board. Hand-copying a 12-digit address is also
// the single easiest thing to get wrong when pairing two boards, and when it
// is wrong both directions die at once with no other symptom.
//
// Discovery removes the manual step. Each board broadcasts a short hello on
// the current channel; the board that hears a hello from the expected role
// adopts the sender as its ESP-NOW peer. The compiled-in ENC_PEER_MAC stays as
// the initial value and fallback, so this is purely additive: a correctly
// configured pair behaves exactly as before, and a misconfigured one now
// repairs itself within a few seconds and says so on the log.
//
// What this does NOT fix
// ----------------------
// A channel or AP mismatch. ESP-NOW frames never leave the radio, so a hello is
// only audible on the channel its sender is associated to. Silently hearing
// nothing is therefore a *diagnosis*, not a bug: it means the two boards are
// on different channels or different APs, which is a network problem. Both
// boards log the AP SSID and channel for exactly this reason.
#ifndef ENCLOSURE_DISCOVERY_H
#define ENCLOSURE_DISCOVERY_H

#include <Arduino.h>
#include <esp_now.h>

#include "enclosure_proto.h"

// Distinct from ENC_PROTO_MAGIC so a hello can never be mistaken for a data
// packet (they differ in length too, but the magic is the cheap first check).
#define ENC_DISCOVERY_MAGIC 0x4E44 // "DN"

// How often to broadcast a hello, and how often to summarise what was heard.
#define ENC_DISCOVERY_INTERVAL_MS 2000UL
#define ENC_DISCOVERY_REPORT_MS 10000UL

enum EnclosureRole : uint8_t
{
    ENC_ROLE_CYD = 0,
    ENC_ROLE_C3 = 1,
};

// Packed to exactly 4 bytes with no padding: an unexpected layout must fail the
// build on both boards rather than produce a hello nobody parses.
#pragma pack(push, 1)
struct EnclosureDiscovery
{
    uint16_t magic;   // ENC_DISCOVERY_MAGIC
    uint8_t role;     // EnclosureRole
    uint8_t channel;  // sender's current WiFi channel, for mismatch reports
};
#pragma pack(pop)

static_assert(sizeof(EnclosureDiscovery) == 4, "EnclosureDiscovery must stay 4 bytes on the wire");

// Validates an inbound hello. Returns true and writes the sender's details when
// the packet is a well-formed hello from the *other* board's role, i.e. one we
// are willing to pair with. Self-hellos and other roles are rejected.
inline bool enclosureDiscoveryParse(const uint8_t *data, int len, uint8_t selfRole,
                                   uint8_t *outRole, uint8_t *outChannel)
{
    if (len != (int)sizeof(EnclosureDiscovery))
        return false;
    const EnclosureDiscovery *d = (const EnclosureDiscovery *)data;
    if (d->magic != ENC_DISCOVERY_MAGIC)
        return false;
    if (d->role != ENC_ROLE_CYD && d->role != ENC_ROLE_C3)
        return false;
    if (d->role == selfRole)
        return false; // our own broadcast looping back
    *outChannel = d->channel;
    *outRole = d->role;
    return true;
}

#endif // ENCLOSURE_DISCOVERY_H
