#include "octo_endpoint.h"

#include <WiFi.h>
#include <Preferences.h>

#include "const.h"
#include "octo_poller.h"
#include "secrets.h"

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

static secrets sec;
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

bool octoEndpointProbe(IPAddress ip, uint16_t port, unsigned long timeoutMs,
                       String &reason)
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
  client.print(sec.getOctoprintApiKey());
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
    reason = "answered HTTP " + String(code) +
             (code == 401 ? " (the API key in secrets.h is wrong for that server)"
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