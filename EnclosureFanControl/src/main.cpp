
#include "main.h"
#include "Arduino.h"


secrets sec = secrets();
OctoPrinter prusa(sec.getOctoprintApiKey(), OCTOTPRINT_IP, OCTOTPRINT_PORT);
ServoLouver louver = ServoLouver();
TemperatureRegulation tempRegulation(&prusa, &louver);

void setup()
{
  Serial.begin(115200);
  Serial.println("Starting up...");
  WiFiManager wm;

  bool res;
  Serial.println("Connecting to WiFi...");
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

  louver.updateForPrinter(prusa.printing(), prusa.filamentName());

  // Update temperature regulation
  tempRegulation.update();

  delay(2000);
}
