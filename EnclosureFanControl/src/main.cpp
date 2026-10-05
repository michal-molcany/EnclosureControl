
#include "main.h"
#include "Arduino.h"
#include <ArduinoOTA.h>

#include "HttpOta.h"

ServoLouver louver = ServoLouver();
TemperatureRegulation tempRegulation(&louver);
ChamberDisplay display;
static secrets sec;

// Last-good snapshot for keep-last-good regulation + display/telemetry.
// Updated non-blockingly from the background poll task.
static OctoSnapshot lastGood;
static bool haveGood = false;
static unsigned long lastUiMs = 0;

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
                 SERIAL_DEBUG != 0, otaStatus);

  // Network lives on the background task from here on; loop only copies
  // snapshots and never blocks on a socket (CYD-style).
  octoPollerStart();
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

  espnowLinkBegin(ENC_PEER_MAC);
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
