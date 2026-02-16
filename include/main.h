#ifndef MAIN_H
#define MAIN_H

#include "Free_Fonts.h"
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <TFT_eWidget.h>
#include "secrets.h"
#include "const.h"
#include "screen.h"
#include <OctoPrinter.h>
#include <WiFiManager.h>

// Function declarations
void UpdateScreen();

void btnPreheet230_pressAction(void);
void setup();
void loop();

// External variables that might be needed in other files
extern WiFiClient client;
extern IPAddress ip;
extern long api_lasttime;
extern bool pressed;
extern bool released;
extern XPT2046_Touchscreen touchscreen;
extern ButtonWidget btnPreheet215;
extern ButtonWidget btnPreheet230;
extern ButtonWidget *btn[];
extern uint8_t buttonCount;
extern int x, y, z;
extern OctoPrinter prusa;

#endif // MAIN_H