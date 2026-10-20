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
// `apiKey` is the credential to PROBE WITH, not necessarily the one in force:
// that is what lets a candidate key be tested before it is stored. Callers must
// have validated it (no CR/LF/control bytes) - it goes into a raw header line
// here. Null is treated as empty.
//
// Returns true when the server answered 2xx. On failure `reason` gets a short
// human-readable cause ("connect failed", "timeout", "HTTP 401"). Blocking, and
// only ever called from an explicit user action.
bool octoEndpointProbe(IPAddress ip, uint16_t port, const char *apiKey,
                       unsigned long timeoutMs, String &reason);

// Stores an override and repoints the poll task. Assumes the caller has already
// probed successfully. `ip` is written as given; validation is the caller's.
void octoEndpointSet(IPAddress ip, uint16_t port);

// Drops the override, returning to the compiled-in OCTOPRINT_IP/PORT.
void octoEndpointClear();

// ---------------------------------------------------------------------------
// OctoPrint API key, same ownership model as the address above.
// ---------------------------------------------------------------------------

// The key in force right now: the stored override if there is one, else the
// compiled-in secrets::getOctoprintApiKey(). Every reader asks here, so there is
// exactly one answer to "what credentials is this board using".
String octoEndpointApiKey();

// True when a stored override is shadowing the compiled-in secrets.h value.
// Shown on the page so "the page says one thing and my firmware says another"
// is answerable.
bool octoEndpointApiKeyOverridden();

// Stores an override and hands it to the poll task. The caller must already
// have probed successfully - a wrong key reads as invalid OctoPrint data, which
// trips the cooling fail-safe, exactly as a wrong address does.
void octoEndpointSetApiKey(const char *key);

// Drops the stored key, returning to the compiled-in secrets.h value. Also
// pushes it to the poll task, so "reset" really does restore the built-in one
// rather than leaving the last entered key in force.
void octoEndpointClearApiKey();

// How long after a change the key is repeated in telemetry frames before the
// sparse cadence takes over, and how often it is refreshed while nothing has
// changed. See octo_endpoint.cpp for why the key is not sent in every frame.
//
// Returns the ENC_KEYFLAG_* value the next telemetry frame should carry, and
// records that it was used - so the caller is expected to call this ONCE per
// frame it actually sends, not speculatively.
//
//   0                  -> carry nothing; the receiver keeps what it has
//   ENC_KEYFLAG_PRESENT-> carry octoEndpointApiKey()
//   ENC_KEYFLAG_CLEAR  -> tell the receiver to drop its override
uint8_t octoEndpointApiKeyClaimFrame();

#endif // OCTO_ENDPOINT_H