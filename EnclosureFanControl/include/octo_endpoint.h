#ifndef OCTO_ENDPOINT_H
#define OCTO_ENDPOINT_H

#include <Arduino.h>
#include <IPAddress.h>

// The OctoPrint address this board polls, and the one it tells the CYD about.
//
// ONE OWNER, ONE DIRECTION
// ------------------------
// The C3 owns the address. It is the board that runs the Wi-FiManager portal and
// serves the web UI, so it is the only one a user can sensibly configure. The
// CYD receives it and stores it, but never originates it.
//
// Why runtime at all: the OctoPrint server's address is the one setting most
// likely to change (a DHCP lease moving, a rebuilt Pi, a second printer on the
// network). It used to live only in include/octoprint_endpoint.h, a git-ignored
// header both firmwares include - which meant a new address needed a rebuild and
// a reflash of BOTH boards, and the usual fix was to leave a wrong value in a
// header nobody remembered existed. The header is still the FALLBACK: with no
// runtime override stored, both boards behave exactly as before.
//
// The compiled-in header is deliberately still required by const.h on both
// boards. It is the value a fresh or cleared board uses, so removing it would
// leave a board that had never been configured through the UI with no address
// at all and no error explaining why.

// Loads any stored override. Call once at boot, BEFORE the poll task starts, so
// the very first poll already goes to the right server.
void octoEndpointBegin();

// Tells this module the poll task now exists, so later changes can reach it.
// Call right after octoPollerStart(). Before that the address is baked into the
// OctoPrinter instance by its constructor and there is nothing to update.
void octoEndpointAttachPoller();

// Address in force right now: the stored override if there is one, else the
// compiled-in OCTOPRINT_IP/PORT. This is what every other reader should use -
// the page, the telemetry, and the poller - so there is exactly one answer to
// "what is this board pointed at".
IPAddress octoEndpointIp();
uint16_t octoEndpointPort();

// True when a runtime override is stored, i.e. the compiled-in default is
// currently being ignored. The page shows this rather than hiding it, because
// "the page says .88 but my firmware says .80" is otherwise unanswerable.
bool octoEndpointOverridden();

// Probes `ip:port` for a live OctoPrint with GET /api/version, bounded by
// `timeoutMs`. Used to refuse a bad address BEFORE it is stored, so a typo
// cannot leave the board polling nothing - which reads as invalid data and
// trips the cooling fail-safe.
//
// Returns true when the server answered 2xx. On failure `reason` gets a short
// human-readable cause ("connect failed", "timeout", "HTTP 401"). Blocking, and
// only ever called from an explicit user action.
bool octoEndpointProbe(IPAddress ip, uint16_t port, unsigned long timeoutMs,
                       String &reason);

// Stores an override and repoints the poll task. Assumes the caller has already
// probed successfully. `ip` is written as given; validation is the caller's.
void octoEndpointSet(IPAddress ip, uint16_t port);

// Drops the override, returning to the compiled-in OCTOPRINT_IP/PORT.
void octoEndpointClear();

#endif // OCTO_ENDPOINT_H