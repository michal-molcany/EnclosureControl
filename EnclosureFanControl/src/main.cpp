
#include "main.h"
#include "Arduino.h"


secrets sec = secrets();
#ifdef OCTOPRINT_IP
OctoPrinter prusa(sec.getOctoprintApiKey(), OCTOPRINT_IP, OCTOPRINT_PORT);
#else
OctoPrinter prusa(sec.getOctoprintApiKey(), OCTOTPRINT_IP, OCTOTPRINT_PORT);
#endif
ServoLouver louver = ServoLouver();
TemperatureRegulation tempRegulation(&prusa, &louver);
ChamberDisplay display;

static bool octoDataValid()
{
  if (prusa.closedOrError())
    return false;
  return !(prusa.toolActual() == 0 && prusa.bedActual() == 0 && prusa.chamberActual() == 0);
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

  prusa.begin();

  louver.begin();

  tempRegulation.begin();
}

void loop()
{
  prusa.update();

  // Single louvre owner: TemperatureRegulation drives fan + servo.
  // (Calling louver.updateForPrinter() here as well would fight it.)

  // Update temperature regulation
  tempRegulation.update();

  // Show chamber temperature + state on the onboard OLED.
  display.update(prusa.chamberActual(), octoDataValid(),
                 tempRegulation.getCurrentState(), tempRegulation.isFanActive());

  delay(2000);
}
