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

// void btnPreheat215_pressAction(void)
// {
//   Serial.println("Preheat 215°C pressed");
//   prusa.preheat(215);
// }

// void btnPreheat230_pressAction(void)
// {
//   Serial.println("Preheat 230°C pressed");
//   prusa.preheat(230);
// }

// void btnPreheatOff_pressAction(void)
// {
//   Serial.println("Preheat Off pressed");
//   prusa.preheatOff();
// }

void UpdateScreen()
{
  if (millis() - api_lasttime > API_REFRESH_TIME || api_lasttime == 0)
  {
    screen.drawWiFiSignal(WiFi.RSSI());
    api_lasttime = millis();
    screen.updateMainScreen(prusa);
  }
}

void setup()
{
  Serial.begin(115200);

  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);
  touchscreen.setRotation(3);

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

    for (int i = 0; i < 2; i++)
    {
      if (screen.btnArray[i]->contains(x, y))
      {
        screen.btnArray[i]->press(true);
        screen.btnArray[i]->drawButton(true);
        delay(200); // Add a small delay to ensure the button press is visually registered

        screen.btnArray[i]->pressAction();
        screen.btnArray[i]->drawButton(false); // Redraw the button in its normal state after the press
        screen.btnArray[i]->press(false);      // Reset the button state to not pressed
        Serial.println("Button pressed");
      }
      else
      {
        screen.btnArray[i]->press(false);
        screen.btnArray[i]->drawButton(false);
        screen.btnArray[i]->releaseAction();
        Serial.println("Button not pressed");
      }
    }
    // bool pressed = myBtn.contains(x, y);
    // if(pressed) {
    //   myBtn.press(true);
    //   myBtn.pressAction();
    //   Serial.println("Button pressed");
    // } else {
    //   myBtn.press(false);
    //   myBtn.releaseAction();
    //   Serial.println("Button not pressed");
    // }

    // Serial.print("X: ");
    // Serial.print(x);
    // Serial.print(" Y: ");
    // Serial.println(y);

    // pressed = true;
    // released = false;
    // Serial.println("Pressed");
  }
  else
  {
    pressed = false;
  }

  UpdateScreen();

  if (!released)
  {
  }

  // Update button inversion states
  screen.updateButtonStates();
}
