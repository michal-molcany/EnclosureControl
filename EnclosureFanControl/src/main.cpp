
#include "main.h"
#include "Arduino.h"
#include <ArduinoOTA.h>
#include <string.h>

#include "HttpOta.h"
#include "enclosure_api_server.h"
#include "enclosure_ui.h"
#include "enclosure_version.h"
#include "octo_endpoint.h"
#include <ESPmDNS.h>
#include <Preferences.h>

ServoLouver louver = ServoLouver();
TemperatureRegulation tempRegulation(&louver);
ChamberDisplay display;
static secrets sec;

// Last-good snapshot for keep-last-good regulation + display/telemetry.
// Updated non-blockingly from the background poll task.
static OctoSnapshot lastGood;
static bool haveGood = false;
static unsigned long lastUiMs = 0;

// Latest chamber reading, mirrored here so the enclosure-API telemetry callback
// (a free function with no access to loop() locals) reports the same value the
// OLED and ESP-NOW telemetry use. Refreshed every loop iteration.
static double g_chamberTemp = 0.0;
static bool g_chamberValid = false;

// Serves GET /api/enclosure. The CYD requests this only while its ESP-NOW link
// is stale, so this path never runs once the radios agree on a channel again.
static EnclosureApi::Telemetry provideEnclosureTelemetry()
{
  EnclosureApi::Telemetry t;
  t.louvrePct = tempRegulation.louvrePercent();
  t.fanPct = tempRegulation.fanPercent();
  t.chamberCx10 = g_chamberValid ? (int16_t)(g_chamberTemp * 10.0) : ENC_CHAMBER_INVALID;
  t.state = tempRegulation.stateCode();
  t.mode = tempRegulation.controlModeCode();
  // Carried over this transport too, so the CYD still learns a new OctoPrint
  // address while it is falling back to HTTP because ESP-NOW is unusable.
  const IPAddress octo = octoEndpointIp();
  t.octoIp[0] = octo[0];
  t.octoIp[1] = octo[1];
  t.octoIp[2] = octo[2];
  t.octoIp[3] = octo[3];
  t.octoPort = octoEndpointPort();
  // The API key, on the same on-change/refresh cadence as the ESP-NOW copy and
  // with the same ENC_KEYFLAG_* meaning. Carried here too so the CYD still
  // receives a key changed through the web UI while the radio link is down -
  // the endpoint hand-off above already works this way, and a setting that
  // reached the display over one transport but not the other would be worse
  // than not carrying it at all.
  t.octoKeyFlags = octoEndpointApiKeyClaimFrame();
  if (t.octoKeyFlags & ENC_KEYFLAG_PRESENT)
  {
    const String key = octoEndpointApiKey();
    strncpy(t.octoKey, key.c_str(), ENC_OCTO_KEY_MAX - 1);
    t.octoKey[ENC_OCTO_KEY_MAX - 1] = '\0';
  }
  return t;
}

// Applies a command POSTed to /api/enclosure. applyRemoteCommand() refreshes
// remoteLinkAlive(), so the "linked" footer and the command-loss fallback behave
// over the API exactly as they do over ESP-NOW.
static void applyEnclosureApiCommand(const EnclosureCommand &cmd)
{
  tempRegulation.applyRemoteCommand(cmd);
}

// ---------------------------------------------------------------------------
// Runtime PID gains: NVS persistence + the /api/pid callbacks.
//
// The C3 is the single source of truth for the gains. The OctoPrint plugin
// reads them over HTTP and writes them back; it never keeps its own copy as the
// authority. That way a tuning cannot exist in two places and disagree - the
// exact failure the shared headers exist to prevent elsewhere in this repo.
// ---------------------------------------------------------------------------

static Preferences pidPrefs;
static const char *PID_NS = "enclosure";

// Guards the gain set as a unit. An absent (or older) version means "no stored
// tuning", so a blank or half-written NVS falls back to the compiled-in defaults
// rather than leaving the regulator on a partial tuning.
static const char *PID_KEY_VER = "pidver";
static const uint8_t PID_VERSION = 1;

static void pidReport(const char *what)
{
  const PidGains g = tempRegulation.gains();
  Serial.printf("[PID] %s kp=%.3f ki=%.3f kd=%.3f\n", what,
                (double)g.kp, (double)g.ki, (double)g.kd);
}

static void pidWrite()
{
  const PidGains g = tempRegulation.gains();
  if (!pidPrefs.begin(PID_NS, false))
  {
    Serial.println("[PID] NVS open for WRITE failed - gains NOT saved (still live in RAM)");
    return;
  }
  // Values are written before the version stamp, so a power cut mid-save leaves
  // the OLD version stamp in place and the partial set is ignored on next boot.
  pidPrefs.putFloat("kp", g.kp);
  pidPrefs.putFloat("ki", g.ki);
  pidPrefs.putFloat("kd", g.kd);
  pidPrefs.putUChar(PID_KEY_VER, PID_VERSION);
  pidPrefs.end();
  Serial.println("[PID] gains saved to NVS");
}

static void pidLoadAtBoot()
{
  PidGains g = pidDefaultGains();
  if (pidPrefs.begin(PID_NS, true))
  {
    if (pidPrefs.getUChar(PID_KEY_VER, 0) == PID_VERSION)
    {
      PidGains stored = {
          pidPrefs.getFloat("kp", g.kp),
          pidPrefs.getFloat("ki", g.ki),
          pidPrefs.getFloat("kd", g.kd)};
      // Clamp rather than reject: a stored value outside the accepted range is
      // recoverable, and falling back to the defaults would silently discard a
      // tuning the user believes is active.
      pidClampGains(stored);
      g = stored;
      pidReport("restored from NVS");
    }
    else
    {
      pidReport("no stored tuning, using defaults");
    }
    pidPrefs.end();
  }
  else
  {
    pidReport("NVS unavailable, using defaults");
  }
  // setGains() re-validates, so this is the last line of defence.
  tempRegulation.setGains(g);
}

static void pidRestoreDefaults()
{
  tempRegulation.setGains(pidDefaultGains());
  pidReport("restored defaults");
  pidWrite();
}

static PidGains provideEnclosurePid()
{
  return tempRegulation.gains();
}

static bool applyEnclosurePid(const PidGains &g)
{
  return tempRegulation.setGains(g);
}

// True when the NVS copy of the gains still matches the running ones - i.e.
// nothing applied through the web UI is one reboot away from being silently lost.
//
// Read-only Preferences open per call, on a request that happens at most every
// 2 s: NVS wear is not the concern here (three float reads write nothing), and a
// cached copy could disagree with a `save` that happened through curl.
static bool pidGainsAreSaved()
{
  if (!pidPrefs.begin(PID_NS, true))
    return true; // no storage to disagree with
  const bool matches = pidPrefs.getUChar(PID_KEY_VER, 0) == PID_VERSION &&
                       pidPrefs.getFloat("kp", -1) == tempRegulation.gains().kp &&
                       pidPrefs.getFloat("ki", -1) == tempRegulation.gains().ki &&
                       pidPrefs.getFloat("kd", -1) == tempRegulation.gains().kd;
  pidPrefs.end();
  return matches;
}

// ---------------------------------------------------------------------------
// Web UI status provider.
//
// Reads the same values the OLED and the ESP-NOW telemetry use, mirrored as
// globals by loop() because this is a free function with no access to loop()
// locals. A browser is a second observer, never a second authority: everything
// here is read-only, and every write goes through the same callbacks the
// enclosure API uses.
// ---------------------------------------------------------------------------

static EnclosureUi::Status provideEnclosureUiStatus()
{
  EnclosureUi::Status s;
  s.chamberCx10 = g_chamberValid ? (int16_t)(g_chamberTemp * 10.0) : ENC_CHAMBER_INVALID;
  s.state = tempRegulation.stateCode();
  s.mode = tempRegulation.controlModeCode();
  s.louvrePct = tempRegulation.louvrePercent();
  s.fanPct = tempRegulation.fanPercent();
  s.fanActive = tempRegulation.isFanActive();
  s.manualLouvrePct = tempRegulation.manualLouvrePercent();
  s.manualFanPct = tempRegulation.manualFanPercent();
  s.jobActive = tempRegulation.jobIsActive();
  s.material = tempRegulation.activeMaterial();
  s.setpointC = (int8_t)tempRegulation.activeSetpoint();
  s.emergencyActive = tempRegulation.isEmergencyActive();
  s.remotePeer = espnowLinkHasPeer();
  s.remoteLinkAlive = tempRegulation.remoteLinkAlive();
  s.gainsSaved = pidGainsAreSaved();
  s.octoIp = octoEndpointIp();
  s.octoPort = octoEndpointPort();
  s.octoOverridden = octoEndpointOverridden();
  s.octoKeySet = octoEndpointApiKey().length() > 0;
  s.octoKeyOverridden = octoEndpointApiKeyOverridden();
  // Reported as "the CYD is being told", never as "the CYD has it": telemetry
  // is one-way, so this board cannot know what the far end stored.
  s.octoSentToCyd = espnowLinkHasPeer();
  return s;
}

// A manual command from the browser. Deliberately applyLocalCommand(), not
// applyRemoteCommand(): a browser click is not proof that the CYD is alive, and
// stamping the link liveness from here would make the OLED claim a connection
// that does not exist.
static void applyEnclosureUiManual(const EnclosureCommand &cmd)
{
  tempRegulation.applyLocalCommand(cmd);
}

// ---------------------------------------------------------------------------
// OctoPrint endpoint, from the web UI.
//
// The C3 owns this address; the CYD is told. Setting it goes through a PROBE
// first: a wrong address makes this board see invalid OctoPrint data, which
// trips the cooling fail-safe and silently stops cooling the enclosure. Refusing
// a bad address at the point of entry is much cheaper than discovering it from a
// cold chamber later.
// ---------------------------------------------------------------------------

// How long a candidate address gets to answer. Long enough for a loaded Pi
// running OctoPrint on an SD card, short enough that a mistyped address is
// reported immediately rather than appearing to hang.
static const unsigned long OCTO_PROBE_TIMEOUT_MS = 2500;

static bool applyEnclosureUiEndpoint(IPAddress ip, uint16_t port, const char *key,
                                     String &reason)
{
  reason = "";
  // A pasted key is the candidate; a blank field means "keep the one in force".
  // Either way the probe runs against the CANDIDATE ADDRESS with the CANDIDATE
  // KEY, which is the only combination that answers the question being asked.
  const String probeKey = (key && key[0]) ? String(key) : octoEndpointApiKey();
  if (!octoEndpointProbe(ip, port, probeKey.c_str(), OCTO_PROBE_TIMEOUT_MS, reason))
    return false;

  // Only now that the pair is known good is anything stored. Setting the key
  // first publishes it to the poll task and to the CYD, so doing it after the
  // address means the board is never briefly pointing at the new server with
  // the old credentials.
  if (key && key[0] && String(key) != octoEndpointApiKey())
    octoEndpointSetApiKey(key);
  octoEndpointSet(ip, port);
  return true;
}

// Restore the compiled-in include/octoprint_endpoint.h address AND the
// secrets.h key. No probe: the point of this button is to get back to a state
// that is known to have worked. Both are cleared together because the page sets
// them as a pair - see applyEnclosureUiEndpoint() for why they cannot be set
// independently.
static void resetEnclosureUiEndpoint()
{
  octoEndpointClear();
  octoEndpointClearApiKey();
}

// Suppresses the normal chamber repaint so the OLED shows OTA progress (and
// lingers on a failure) instead of being overwritten every second.
static bool otaActive = false;
static unsigned long otaErrorMs = 0;

// The 72x40 screen has no room for an error code, and the commonest failure is
// a wrong password, so the shared lib reports a short name we can show as-is.
static void otaStatus(const char *text)
{
  display.showMessage("HOTA", text);
}

static bool otaHoldsDisplay()
{
  if (otaActive)
    return true;
  if (HttpOta::busy())
    return true;
  if (otaErrorMs != 0 && (millis() - otaErrorMs) < OTA_ERROR_HOLD_MS)
    return true;
  otaErrorMs = 0;
  return false;
}

// The 5x7 font on a 72px screen has no room for "err 0", and a bare 0 is the
// most common failure of all, so name the codes instead.
static const char *otaErrorName(ota_error_t err)
{
  switch (err)
  {
  case OTA_AUTH_ERROR:
    return "auth";
  case OTA_BEGIN_ERROR:
    return "begin";
  case OTA_CONNECT_ERROR:
    return "connect";
  case OTA_RECEIVE_ERROR:
    return "recv";
  case OTA_END_ERROR:
    return "end";
  default:
    return "?";
  }
}

static void otaBegin()
{
  ArduinoOTA.setHostname(OTA_ARDUINO_HOSTNAME);
  const String otaPass = sec.getOTAPassword();
  if (otaPass.length() > 0)
  {
    ArduinoOTA.setPassword(otaPass.c_str());
  }
#if SERIAL_DEBUG
  else
  {
    Serial.println("[OTA] WARNING: no password set - OTA is open to anyone on the LAN");
  }
#endif

  ArduinoOTA.onStart([]()
                     {
                       otaActive = true;
                       otaErrorMs = 0;
                       display.showMessage("OTA", "starting");
                     });
  ArduinoOTA.onProgress([](unsigned int cur, unsigned int total)
                        {
                          if (total == 0)
                            return;
                          display.showMessage("OTA", String(cur * 100UL / total) + "%");
                        });
  ArduinoOTA.onEnd([]()
                   {
                     display.showMessage("OTA", "done reboot");
                   });
  ArduinoOTA.onError([](ota_error_t err)
                     {
                       otaActive = false;
                       otaErrorMs = millis();
                       display.showMessage("OTA", otaErrorName(err));
                       Serial.printf("[OTA] failed: %s (%d)\n", otaErrorName(err),
                                     static_cast<int>(err));
                     });

  ArduinoOTA.begin();
#if SERIAL_DEBUG
  Serial.printf("[OTA] listening on %s.local:3232 (auth=%u)\n",
                OTA_ARDUINO_HOSTNAME, otaPass.length() > 0 ? 1 : 0);
#endif
}

void setup()
{
  Serial.begin(115200);
  // Native USB needs time to enumerate; bounded wait only (never hang headless).
  unsigned long serialT0 = millis();
  while (!Serial && (millis() - serialT0) < 2000)
  {
    delay(50);
  }
  delay(500);
  // Identify the build and which board it is on. Printed first, before anything
  // that can block on the network, so a serial capture alone answers "what is
  // actually running here?" even if boot later stalls.
  printFirmwareVersion("C3");
  Serial.println("Starting up...");
  Serial.flush();

  display.begin(); // splash visible even if WiFi portal blocks below

  WiFiManager wm;
  wm.setConnectTimeout(30);
  wm.setConfigPortalTimeout(180);

  bool res;
  Serial.println("Connecting to WiFi...");
  display.showMessage("WiFi...", "connecting");
  res = wm.autoConnect("EnclosureFanControl"); // password protected ap
  if (!res)
  {
    Serial.println("Failed to connect");
    delay(3000);
    ESP.restart();
  }
  else
  {
    Serial.println("Connected to WiFi");
    Serial.println(WiFi.localIP());
  }
  WiFi.setSleep(false); // mains-powered: minimize ESP-NOW latency

  // OTA must start only after WiFiManager's blocking portal has handed
  // networking back; it owns the AP until autoConnect() returns.
  otaBegin();
  HttpOta::begin(OTA_HTTP_PORT, OTA_HTTP_USER, sec.getOTAPassword().c_str(),
                 SERIAL_DEBUG != 0, otaStatus, "C3");

  // Network lives on the background task from here on; loop only copies
  // snapshots and never blocks on a socket (CYD-style).
  //
  // The OctoPrint address is loaded BEFORE the poll task starts, so the very
  // first poll already goes to the right server instead of failing once against
  // the compiled-in default.
  octoEndpointBegin();
  octoPollerStart();
  octoEndpointAttachPoller();
  display.showMessage("OctoPrint", "connecting");
  const uint32_t bootStart = millis();
  while (!octoPollerHasData() && (millis() - bootStart < 20000))
  {
    delay(100);
  }

  louver.begin();

  // Power-on self-test: open 25% and close again to verify the servo.
  display.showMessage("Louvre", "test 25%");
  louver.selfTest();

  tempRegulation.begin();

  // Stored gains must be in force BEFORE the first regulation cycle, not after:
  // begin() leaves the PID on the compiled-in defaults, so loading later would
  // let a boot with no valid OctoPrint data run briefly on the wrong tuning.
  pidLoadAtBoot();

  espnowLinkBegin(ENC_PEER_MAC);

  // Enclosure API fallback (the CYD polls this only when its ESP-NOW link is
  // stale). Served on its own port, with the shared token from
  // include/enclosure_api.h. The mDNS host is already registered by
  // ArduinoOTA.begin() above, so just advertise the port as an _http._tcp service
  // for the CYD's mDNS queryHost()/browse.
  EnclosureApi::begin(ENC_API_PORT, ENC_API_TOKEN, provideEnclosureTelemetry,
                      applyEnclosureApiCommand, provideEnclosurePid,
                      applyEnclosurePid, pidWrite, pidRestoreDefaults);
  MDNS.addService("http", "tcp", ENC_API_PORT);
#if SERIAL_DEBUG
  // The CYD looks this board up by the mDNS name ArduinoOTA.begin() registered
  // above, which is OTA_ARDUINO_HOSTNAME - the same value as the CYD's
  // ENC_API_MDNS_NAME, but it lives on the side that owns it.
  Serial.printf("[API] enclosure api on port %u, mDNS %s.local, auth=%s\n",
                (unsigned)ENC_API_PORT, OTA_ARDUINO_HOSTNAME,
                ENC_API_TOKEN[0] ? "token" : "OPEN");
#endif

  // Web UI (lib/EnclosureUi) on its own port - see the note on ENC_UI_PORT in
  // const.h for why it is not a path on the API's server. Same token, so the
  // browser prompts once and the page never handles the secret itself.
  EnclosureUi::Config ui;
  ui.port = ENC_UI_PORT;
  ui.token = ENC_API_TOKEN;
  ui.fwVersion = ENC_FW_VERSION_STR;
  ui.status = provideEnclosureUiStatus;
  ui.pid = provideEnclosurePid;
  ui.setPid = applyEnclosurePid;
  ui.savePid = pidWrite;
  ui.restoreDefaultPid = pidRestoreDefaults;
  ui.manual = applyEnclosureUiManual;
  ui.setEndpoint = applyEnclosureUiEndpoint;
  ui.clearEndpoint = resetEnclosureUiEndpoint;
  EnclosureUi::begin(ui);
  // Deliberately NO MDNS.addService() here. The host name is already registered
  // by ArduinoOTA.begin(), which is all a browser needs to resolve
  // http://<host>.local:8082/, and ESPmDNS keys services by name: a second
  // _http._tcp service would replace the 8081 one the CYD's discovery relies on.
#if SERIAL_DEBUG
  Serial.printf("[UI] web ui on port %u, open http://%s.local:%u/ (user: cyd, password: the API token)\n",
                (unsigned)ENC_UI_PORT, OTA_ARDUINO_HOSTNAME, (unsigned)ENC_UI_PORT);
#endif
}

void loop()
{
  // Re-assert the channel pin first, before anything tries to send. This must be
  // at loop rate: the AP pulls the station back onto its own channel on every
  // beacon (~100ms), so maintaining the pin only when the UI block ticks (1Hz)
  // would leave the radio off-channel when telemetry goes out and ESP-NOW would
  // still fail. No-op when ENC_WIFI_CHANNEL is 0.
  espnowLinkMaintainChannel(millis());

  // Non-blocking: services the UDP handshake and image chunks while idle.
  ArduinoOTA.handle();
  HttpOta::poll();
  EnclosureApi::poll();
  // Separate server on its own port, so the browser page cannot delay the CYD's
  // API calls above. Still non-blocking: one handled request per iteration.
  EnclosureUi::poll();

  // Non-blocking copy of the latest poll (20ms mutex try inside).
  // Keep-last-good: regulation/display continue on lastGood when the
  // fresh copy is missing or invalid.
  OctoSnapshot snap;
  if (octoPollerCopy(snap) && octoSnapshotValid(snap))
  {
    lastGood = snap;
    haveGood = true;
  }
  const OctoSnapshot &effective = haveGood ? lastGood : snap;
  const bool valid = haveGood ? octoSnapshotValid(lastGood) : false;
  const double chamberTemp = haveGood ? lastGood.chamberActual : 0.0;
  g_chamberTemp = chamberTemp;
  g_chamberValid = valid;

  // Single louvre owner: TemperatureRegulation drives fan + servo.
  // (Calling louver.updateForPrinter() here as well would fight it.)

  // Update temperature regulation (internally gated to PID_UPDATE_INTERVAL).
  tempRegulation.update(effective);

  // Settle a break-away boost. Must run every iteration: update() is gated to
  // 1Hz, so a 300ms boost could not otherwise be ended on time and the fan
  // would sit at full power for a second on every start. No-op when idle.
  tempRegulation.serviceFan();

  // Show chamber temperature + state on the onboard OLED (~1s, non-blocking).
  // While unpaired, show the MAC pairing screen instead (no serial needed).
  // Linked = peer configured AND commands recently received from the CYD.
  const unsigned long now = millis();
  if (now - lastUiMs >= 1000)
  {
    lastUiMs = now;
    const bool linked = espnowLinkHasPeer() && tempRegulation.remoteLinkAlive();
    // Skip the repaint mid-OTA so it does not overwrite the progress screen.
    if (!otaHoldsDisplay())
    {
      if (!espnowLinkHasPeer())
        display.showPairing(WiFi.macAddress());
      else
        display.update(chamberTemp, valid,
                       tempRegulation.getCurrentState(), tempRegulation.isFanActive(),
                       tempRegulation.controlModeCode() == ENC_MODE_MANUAL, linked,
                       tempRegulation.louvrePercent(), tempRegulation.fanPercent());
    }

    // ESP-NOW telemetry to PrinterDisplay + inbound remote commands.
    espnowLinkPoll(tempRegulation, chamberTemp, valid);
#if SERIAL_DEBUG
    Serial.printf("[DBG] mode=%s state=%s temp=%.1fC valid=%u job=%u louvre=%u fan=%u link=%u\n",
                  tempRegulation.controlModeCode() == ENC_MODE_MANUAL ? "MANUAL" : "AUTO",
                  tempRegulation.getCurrentState().c_str(),
                  chamberTemp,
                  static_cast<unsigned>(valid),
                  static_cast<unsigned>(tempRegulation.jobIsActive()),
                  static_cast<unsigned>(tempRegulation.louvrePercent()),
                  static_cast<unsigned>(tempRegulation.fanPercent()),
                  static_cast<unsigned>(linked));
#endif
  }

  // Idle faster during any update so the OTA/HTTP sockets drain quickly.
  // HttpOta::busy() matters as much as otaActive: the HTTP path would otherwise
  // sit at 50 ms per iteration, throttling a ~1 MB upload to ~700 rounds.
  delay((otaActive || HttpOta::busy()) ? 1 : 50);
}
