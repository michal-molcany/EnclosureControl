#include "octo_endpoint.h"

#include <WiFi.h>
#include <Preferences.h>

#include "const.h"
#include "enclosure_proto.h" // ENC_KEYFLAG_*
#include "octo_poller.h"
#include "secrets.h"

// Declared up here because both the API-key block below and the address block
// further down need it.
static secrets sec;

// ---------------------------------------------------------------------------
// OctoPrint API key.
//
// SEPARATE NVS NAMESPACE FROM THE ADDRESS, and not by tidiness: octoEndpointClear()
// calls prefs.clear() on "octoend", which would wipe the key along with the
// address. The two settings are reset independently in the UI, so sharing a
// namespace would make "reset the address" silently discard a key the user
// believes is still stored.
// ---------------------------------------------------------------------------
static const char *OCTO_KEY_NS = "octokey";
static const char *OCTO_KEY_KEY = "apikey";

// The compiled-in fallback, read once. Read lazily rather than in a static
// initialiser because it constructs a String, and a global String constructed
// before main() runs is a static-init hazard on ESP32 for no benefit.
static String compiledInKey;
static bool compiledInKeyRead = false;

static String compiledKey()
{
  if (!compiledInKeyRead)
  {
    compiledInKey = sec.getOctoprintApiKey();
    compiledInKeyRead = true;
  }
  return compiledInKey;
}

// Key currently in force, and whether it is a stored override.
static String currentKey;
static bool keyOverridden = false;

// Cadence for putting the key into a telemetry frame.
//
// WHY NOT EVERY FRAME
// -------------------
// octoIp is sent every frame because four bytes cost nothing. A credential is
// not four bytes: this is a cleartext radio unicast, so every frame carrying the
// key is another copy of the OctoPrint credential in the air, forever, at 1 Hz.
// So the key rides along only when it has something to say:
//
//   - repeatedly for ANNOUNCE_MS after any change, so a CYD that happens to be
//     mid-reboot (or that misses the first frame) still converges quickly;
//   - then once every REFRESH_MS, so a CYD that was powered off across the
//     change still picks it up without being told again.
//
// The frames in between carry flags == 0, which the receiver treats as "keep
// what you have" - see ENC_KEYFLAG_* in enclosure_proto.h. That third state is
// the whole reason this is safe; without it a sparse cadence would read as a
// stream of "drop your override" and the CYD would flip back to its own key once
// per second.
#define OCTO_KEY_ANNOUNCE_MS 5000UL
#define OCTO_KEY_REFRESH_MS 30000UL

// Set on any change. While it is set, every frame carries the key; once it
// lapses, only the periodic refresh does.
static unsigned long keyAnnounceUntilMs = 0;
// Absolute time of the next refresh frame, so it survives a change landing
// between two ticks without needing its own flag.
static unsigned long nextKeyRefreshMs = 0;

// The single source of truth for "which OctoPrint is this board talking to".
//
// STORAGE
// -------
// NVS, namespace "octoend". Separate from the PID gains' "enclosure" namespace
// on purpose: those are wiped by "restore PID defaults" and these must not be.
// Two keys plus a version stamp, written values-first so a power cut mid-write
// leaves the previous (valid) set in place rather than a half-updated one.
//
// No version stamp needed here, unlike the PID gains: a missing key means "no
// override", which is exactly the right fallback, and there is no
// partially-populated state that could be mistaken for a real setting. An
// override is only "set" when the IP is non-zero AND the port is non-zero.

// NVS namespace names are capped at 15 characters.
static const char *OCTO_NS = "octoend";
static const char *OCTO_KEY_IP = "ip";
static const char *OCTO_KEY_PORT = "port";

static Preferences prefs;

// Current address in force. Seeded from the compiled-in header and overwritten
// by octoEndpointBegin() when an override is stored, so every reader can ask
// without touching NVS.
static IPAddress currentIp;
static uint16_t currentPort = OCTOPRINT_PORT;
static bool overridden = false;

// Applies `currentIp/currentPort` to the poll task. Separate from the setter
// because the poller starts AFTER the first load: before the task exists the
// address is already baked into the OctoPrinter instance by its constructor, and
// calling in then would be a write to a task that has not been created.
static bool pollerRunning = false;

void octoEndpointBegin()
{
  currentIp.fromString(OCTOPRINT_IP);
  currentPort = OCTOPRINT_PORT;
  overridden = false;

  // An unreadable NVS is not fatal: the compiled-in default still works, and
  // saying so beats refusing to boot over a cosmetic preference.
  if (prefs.begin(OCTO_NS, true))
  {
    const uint32_t raw = prefs.getUInt(OCTO_KEY_IP, 0);
    const uint16_t port = prefs.getUShort(OCTO_KEY_PORT, 0);
    prefs.end();
    if (raw != 0 && port != 0)
    {
      currentIp = IPAddress(raw);
      currentPort = port;
      overridden = true;
    }
  }

  Serial.print(F("[octo] endpoint "));
  Serial.print(currentIp);
  Serial.print(':');
  Serial.print(currentPort);
  Serial.println(overridden ? F(" (stored override)") : F(" (compiled default)"));

  // The key, same override-or-default rule as the address above.
  currentKey = compiledKey();
  keyOverridden = false;
  if (prefs.begin(OCTO_KEY_NS, true))
  {
    const String stored = prefs.getString(OCTO_KEY_KEY, "");
    prefs.end();
    if (stored.length() > 0)
    {
      currentKey = stored;
      keyOverridden = true;
    }
  }
  // Never print the key itself - not even at boot, not even on a debug build.
  // The serial log is the most widely shared artefact in a project like this
  // (pasted into an issue, attached to a bug report), so it reports the
  // credential's provenance and length only.
  Serial.print(F("[octo] api key "));
  Serial.print(keyOverridden ? F("stored override") : F("compiled default"));
  Serial.print(F(" ("));
  Serial.print(currentKey.length());
  Serial.println(F(" chars)"));

  if (pollerRunning)
    octoPollerSetEndpoint(currentIp, currentPort);
}

// Called by the poller once its task exists, so later changes have somewhere to
// go. Without this the very first override set from the web UI would update the
// stored value and the page but never reach the task that does the polling.
void octoEndpointAttachPoller()
{
  pollerRunning = true;
  octoPollerSetEndpoint(currentIp, currentPort);
  octoPollerSetApiKey(currentKey.c_str());
}

IPAddress octoEndpointIp()
{
  return currentIp;
}

uint16_t octoEndpointPort()
{
  return currentPort;
}

bool octoEndpointOverridden()
{
  return overridden;
}

bool octoEndpointProbe(IPAddress ip, uint16_t port, const char *apiKey,
                       unsigned long timeoutMs, String &reason)
{
  reason = "";
  if (WiFi.status() != WL_CONNECTED)
  {
    reason = "this board is not on WiFi";
    return false;
  }

  // Short connect timeout even when the caller allows more: on a LAN a host
  // that is simply absent should fail fast, and the remaining budget is for the
  // server that DID answer taking its time.
  WiFiClient client;
  const uint32_t deadline = millis() + timeoutMs;
  const uint16_t connectMs =
      timeoutMs < 1200 ? (uint16_t)timeoutMs : (uint16_t)1200;
  if (!client.connect(ip, port, connectMs))
  {
    // A refused connection and a silent one mean different things to whoever
    // typed the address, and "no route to host" is the usual symptom of a typo.
    reason = "no answer (wrong address, or OctoPrint is not running?)";
    return false;
  }

  client.setTimeout(2);
  client.print(F("GET /api/version HTTP/1.1\r\n"));
  client.print("Host: ");
  client.print(ip);
  client.print("\r\nX-Api-Key: ");
  // The candidate key, not the one in force: that is the entire point of probing
  // a key before storing it. `apiKey` is validated by the caller (see
  // receiveApiKey in lib/EnclosureUi) - this function must not have to defend
  // against a CR/LF in it, because a newline here ends the header line and
  // everything after it becomes a header of its own.
  client.print(apiKey ? apiKey : "");
  client.print(F("\r\nConnection: close\r\n\r\n"));

  // The status line is all this needs; the body is irrelevant to "is it there".
  String statusLine;
  while (millis() < deadline)
  {
    if (!client.connected() && statusLine.length() == 0)
      break;
    if (client.available())
    {
      const int c = client.read();
      if (c < 0)
        break;
      if (c == '\n')
        break;
      statusLine += (char)c;
    }
  }
  client.stop();

  if (statusLine.length() == 0)
  {
    reason = "connected, but no HTTP response (not an OctoPrint server?)";
    return false;
  }
  // "HTTP/1.1 200 OK" - the code is the three characters after the first space.
  const int sp = statusLine.indexOf(' ');
  int code = 0;
  if (sp > 0)
    code = statusLine.substring(sp + 1).substring(0, 3).toInt();
  if (code < 200 || code >= 300)
  {
    // 401 and 403 are both OctoPrint refusing the credential, and for the user's
    // purposes they mean the same thing: the key does not work on this server.
    reason = "answered HTTP " + String(code) +
             ((code == 401 || code == 403)
                  ? " (the API key was refused by that server)"
                  : "");
    return false;
  }
  return true;
}

void octoEndpointSet(IPAddress ip, uint16_t port)
{
  currentIp = ip;
  currentPort = port;
  overridden = true;
  if (prefs.begin(OCTO_NS, false))
  {
    prefs.putUInt(OCTO_KEY_IP, (uint32_t)ip);
    prefs.putUShort(OCTO_KEY_PORT, port);
    prefs.end();
  }
  Serial.print(F("[octo] endpoint stored "));
  Serial.print(ip);
  Serial.print(':');
  Serial.println(port);
  if (pollerRunning)
    octoPollerSetEndpoint(ip, port);
}

void octoEndpointClear()
{
  overridden = false;
  if (prefs.begin(OCTO_NS, false))
  {
    prefs.clear();
    prefs.end();
  }
  currentIp.fromString(OCTOPRINT_IP);
  currentPort = OCTOPRINT_PORT;
  Serial.println(F("[octo] override cleared, back to the compiled default"));
  if (pollerRunning)
    octoPollerSetEndpoint(currentIp, currentPort);
}

// ---------------------------------------------------------------------------
// API key
// ---------------------------------------------------------------------------

String octoEndpointApiKey()
{
  return currentKey;
}

bool octoEndpointApiKeyOverridden()
{
  return keyOverridden;
}

// Shared by set and clear: apply the new key, hand it to the poll task, and
// start the announce window so the CYD converges quickly.
static void publishApiKey(const String &key, bool overridden)
{
  currentKey = key;
  keyOverridden = overridden;
  if (pollerRunning)
    octoPollerSetApiKey(currentKey.c_str());
  // Repeat the key in every frame for a few seconds (see the cadence notes
  // above), then drop to the periodic refresh.
  keyAnnounceUntilMs = millis() + OCTO_KEY_ANNOUNCE_MS;
}

void octoEndpointSetApiKey(const char *key)
{
  publishApiKey(key ? String(key) : String(), true);
  if (prefs.begin(OCTO_KEY_NS, false))
  {
    prefs.putString(OCTO_KEY_KEY, currentKey);
    prefs.end();
  }
  else
  {
    // Say so rather than letting the page report success: the poll task now has
    // the new key, but it will be gone at the next reboot.
    Serial.println(F("[octo] WARNING key applied but NVS write FAILED - lost on reboot"));
  }
  Serial.print(F("[octo] api key stored ("));
  Serial.print(currentKey.length());
  Serial.println(F(" chars)"));
}

void octoEndpointClearApiKey()
{
  if (prefs.begin(OCTO_KEY_NS, false))
  {
    prefs.clear();
    prefs.end();
  }
  // Re-read the compiled-in value rather than trusting a cached copy: the whole
  // point of this button is to get back to a state known to have worked, and
  // the compiled default is that state.
  publishApiKey(compiledKey(), false);
  Serial.println(F("[octo] api key override cleared, back to the compiled default"));
}

uint8_t octoEndpointApiKeyClaimFrame()
{
  const unsigned long now = millis();

  // Signed subtraction, not `now > deadline`: millis() wraps every ~49 days,
  // and a plain comparison reads a wrapped clock as "the deadline passed 49
  // days ago", which would suppress the announce window forever.
  const bool announcing = (int32_t)(now - keyAnnounceUntilMs) < 0;
  const bool refreshDue = (int32_t)(now - nextKeyRefreshMs) >= 0;

  if (!announcing && !refreshDue)
    return 0;

  // Re-arm BEFORE returning, so a frame that is sent but lost still gets
  // followed by another. Rescheduling on every claimed frame is what makes the
  // announce window repeat rather than fire once.
  nextKeyRefreshMs = now + OCTO_KEY_REFRESH_MS;
  if (announcing)
    keyAnnounceUntilMs = now + OCTO_KEY_ANNOUNCE_MS;

  // CLEAR is the flag on its own: a receiver told "present" and "clear" at once
  // has contradictory instructions, and the sane reading (keep nothing) is the
  // one that matches the sender's actual state here.
  return keyOverridden ? ENC_KEYFLAG_PRESENT : ENC_KEYFLAG_CLEAR;
}