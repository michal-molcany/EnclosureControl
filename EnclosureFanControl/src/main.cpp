
#include "main.h"
#include "Arduino.h"

ServoLouver louver = ServoLouver();
TemperatureRegulation tempRegulation(&louver);
ChamberDisplay display;

// Last-good snapshot for keep-last-good regulation + display/telemetry.
// Updated non-blockingly from the background poll task.
static OctoSnapshot lastGood;
static bool haveGood = false;
static unsigned long lastUiMs = 0;

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

  // Show chamber temperature + state on the onboard OLED (~1s, non-blocking).
  // While unpaired, show the MAC pairing screen instead (no serial needed).
  // Linked = peer configured AND commands recently received from the CYD.
  const unsigned long now = millis();
  if (now - lastUiMs >= 1000)
  {
    lastUiMs = now;
    const bool linked = espnowLinkHasPeer() && tempRegulation.remoteLinkAlive();
    if (!espnowLinkHasPeer())
      display.showPairing(WiFi.macAddress());
    else
      display.update(chamberTemp, valid,
                     tempRegulation.getCurrentState(), tempRegulation.isFanActive(),
                     tempRegulation.controlModeCode() == ENC_MODE_MANUAL, linked,
                     tempRegulation.louvrePercent(), tempRegulation.fanPercent());

    // ESP-NOW telemetry to PrinterDisplay + inbound remote commands.
    espnowLinkPoll(tempRegulation, chamberTemp, valid);
  }

  delay(50);
}
