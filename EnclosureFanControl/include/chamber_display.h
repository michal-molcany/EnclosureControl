#ifndef CHAMBER_DISPLAY_H
#define CHAMBER_DISPLAY_H

#include <Arduino.h>

// 0.42" OLED on the ABRobot ESP32-C3 board:
// SSD1306, 72x40 px, I2C SDA=GPIO5 / SCL=GPIO6, addr 0x3C.
#define OLED_SDA_PIN 5
#define OLED_SCL_PIN 6
#define OLED_I2C_ADDR 0x3C

class ChamberDisplay
{
private:
    unsigned long lastDrawMs;
    bool splashShown;

    static String shortState(const String &state);

public:
    ChamberDisplay();

    // Init display + show splash. Safe to call before WiFi is up.
    void begin();

    // Draw big chamber temp + small status line. Throttled internally.
    // Pass valid=false when OctoPrint data is stale/offline (shows "---").
    void update(double chamberTemp, bool valid, const String &state, bool fanActive);

    // One-off message screen (e.g. WiFi portal notice).
    void showMessage(const String &line1, const String &line2);
};

#endif
