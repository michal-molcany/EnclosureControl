#include "main.h"

WiFiClient client;
IPAddress ip;
long api_lasttime = 0;

secrets sec = secrets();
Screen screen;
bool pressed = false;
bool released = false;

SPIClass touchscreenSPI = SPIClass(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);
int x, y, z;

OctoPrinter prusa(sec.getOctoprintApiKey(), OCTOTPRINT_IP, OCTOTPRINT_PORT);

void UpdateScreen()
{
  if (millis() - api_lasttime > API_REFRESH_TIME || api_lasttime == 0)
  {
    screen.drawWiFiSignal(WiFi.RSSI());
    api_lasttime = millis();
    prusa.update();
    screen.updateMainScreen(prusa);
  }
}

void setup()
{
  Serial.begin(115200);

  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);
  touchscreen.setRotation(1);

  screen = Screen();

  WiFiManager wm;
  bool res;
  res = wm.autoConnect("EnclosureDisplay"); // password protected ap
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
  
  screen.ConnectedToWiFi(WiFi.SSID());
  prusa.begin();
  screen.MainScreen();
  screen.updateMainScreen(prusa);
}

void loop()
{
  if (touchscreen.tirqTouched() && touchscreen.touched())
  {
    TS_Point p = touchscreen.getPoint();
    x = map(p.x, 200, 3700, 1, SCREEN_WIDTH);
    y = map(p.y, 240, 3800, 1, SCREEN_HEIGHT);
    z = p.z;
    Serial.print("X: ");
    Serial.print(x);
    Serial.print(" Y: ");
    Serial.println(y);

    pressed = true;
    released = false;
    Serial.println("Pressed");
  }
  else
  {
    pressed = false;
  }

  UpdateScreen();

  if (!released)
  {
    
  }
}
