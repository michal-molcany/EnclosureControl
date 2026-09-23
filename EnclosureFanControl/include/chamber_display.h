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

    // Draw small chamber temp on top, big louvre/fan in the middle, small
    // link state at the bottom. Throttled internally.
    // Pass valid=false when OctoPrint data is stale/offline (shows "---").
    // In manual mode the middle line shows commanded values ("ML25F60").
    // linked=true shows "+CYD LINK OK", else "-CYD NO LINK".
    void update(double chamberTemp, bool valid, const String &state, bool fanActive,
                bool manual = false, bool linked = false,
                uint8_t louvrePct = 0, uint8_t fanPct = 0);

    // One-off message screen (e.g. WiFi portal notice).
    void showMessage(const String &line1, const String &line2);

    // Pairing screen: shows own WiFi MAC split over two lines so it can be
    // read off the display when serial is unavailable. mac like "AA:BB:..".
    void showPairing(const String &mac);
};

#endif
