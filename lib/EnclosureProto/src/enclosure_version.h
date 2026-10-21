#ifndef ENCLOSURE_VERSION_H
#define ENCLOSURE_VERSION_H

// Firmware version for BOTH boards, in one place.
//
// SHARED deliberately: the CYD (PrinterDisplay) and the C3 (EnclosureFanControl)
// are flashed as a pair, and a version that only one board knows about is worse
// than none - it makes "is this the build I think it is?" unanswerable on half
// the hardware. Both include this header, so bumping the numbers here updates
// both firmwares at once.
//
// HOW TO BUMP: increment PATCH for any change that reaches a device; MINOR for
// a change users will notice (new UI, new link behaviour); MAJOR only when the
// ESP-NOW or HTTP wire format changes in a way that would break a board paired
// with an older build. Both boards must then be flashed - a mismatched pair
// still links, but the protocol assumptions differ.
//
// DO THIS BEFORE FLASHING, NOT AFTER CODING. The version is the only thing that
// makes "what is actually running on this board?" answerable from the device
// itself, so a change that is built and flashed without it ships a board that
// under-reports what it is - and the `proto` label in particular is the first
// thing to check when a link misbehaves. Bump it as part of the change, every
// time, not as a follow-up.
//
// Each board prints its own version at boot over serial, prefixed with its role,
// so the line identifies both the build AND which board printed it:
//
//   [FW] CYD firmware v0.3.0 (proto v1, built Oct 12 2026 13:04:05)
//   [FW] C3 firmware v0.3.0 (proto v1, built Oct 12 2026 13:04:07)
//
// The build timestamp comes from the compiler's __DATE__/__TIME__ and is
// deliberately NOT part of the semantic version, so two builds of the same
// version number are still distinguishable on the device.

#define ENC_FW_VERSION_MAJOR 0
#define ENC_FW_VERSION_MINOR 5
#define ENC_FW_VERSION_PATCH 0

// Protocol revision, bumped only when the structs in enclosure_proto.h or the
// JSON keys in lib/EnclosureApi change in a backward-incompatible way.
// Bumped to 2 when EnclosureTelemetry grew the OctoPrint endpoint fields
// (octoIp + octoPort), taking it from 10 to 16 bytes. Bumped to 3 when it grew
// the OctoPrint API key (octoKey + octoKeyFlags), taking it from 16 to 82 bytes,
// and when /api/enclosure gained the matching octoKey/octoKeyFlags JSON keys.
// Both boards check the packet LENGTH against their own sizeof(), so a C3 and a
// CYD on mismatched builds drop every frame rather than misreading it - the link
// goes quiet until both are flashed. Reflash both when this moves.
//
// Note this number is a DIAGNOSTIC LABEL, not an enforced handshake: nothing on
// the link compares it, and the length check does that work implicitly. Its whole
// value is that "proto vN" on a boot line names the layout, so a reader chasing a
// dead link can rule a version mismatch in or out immediately. That only helps if
// it is actually correct.
#define ENC_PROTO_VERSION 3

// Stringify the numbers above so the printed line cannot drift from them.
#define ENC_STR2(x) #x
#define ENC_STR(x) ENC_STR2(x)
#define ENC_FW_VERSION_STR                                             \
    ENC_STR(ENC_FW_VERSION_MAJOR) "." ENC_STR(ENC_FW_VERSION_MINOR)   \
        "." ENC_STR(ENC_FW_VERSION_PATCH)

#ifdef __cplusplus
// One call so both boards cannot print the version differently.
#include <Arduino.h>
inline void printFirmwareVersion(const char *role)
{
    // `role` is a runtime const char*, so it CANNOT go through F()/PSTR(), which
    // require a string literal - hence the plain Serial.print() for that one
    // argument. The fixed fragments use F() to stay in flash, which matters on
    // the DRAM-tight CYD.
    //
    // __DATE__ and __TIME__ are predefined by the compiler and already expand to
    // string literals, so the build stamp needs no -D flag.
    Serial.print(F("[FW] "));
    Serial.print(role);
    Serial.print(F(" firmware v" ENC_FW_VERSION_STR " (proto v"
                  ENC_STR(ENC_PROTO_VERSION) ", built "));
    Serial.print(__DATE__);
    Serial.print(F(" "));
    Serial.print(__TIME__);
    Serial.println(F(")"));
}
#endif

#endif // ENCLOSURE_VERSION_H