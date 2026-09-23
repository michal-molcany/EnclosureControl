#include "chamber_display.h"
#include <U8g2lib.h>

// Native 72x40 constructor - no pixel offsets needed.
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL_PIN, OLED_SDA_PIN);

#define DISPLAY_UPDATE_MIN_MS 1000UL

ChamberDisplay::ChamberDisplay() : lastDrawMs(0), splashShown(false)
{
}

void ChamberDisplay::begin()
{
    u8g2.begin();
    u8g2.setContrast(255);
    u8g2.setBusClock(400000);
    showMessage("Enclosure", "Fan Ctrl");
    splashShown = true;
    lastDrawMs = millis();
}

String ChamberDisplay::shortState(const String &state)
{
    if (state == "PLA_COOLING")
        return "PLA";
    if (state == "PETG_COOLING")
        return "PETG";
    if (state == "ASA_ABS_COOLING")
        return "ASA";
    return "IDLE";
}

void ChamberDisplay::update(double chamberTemp, bool valid, const String &state, bool fanActive)
{
    unsigned long now = millis();
    if (splashShown && (now - lastDrawMs) < DISPLAY_UPDATE_MIN_MS)
        return;
    splashShown = false;
    lastDrawMs = now;

    char tempBuf[10];
    if (valid)
        snprintf(tempBuf, sizeof(tempBuf), "%.1fC", chamberTemp);
    else
        snprintf(tempBuf, sizeof(tempBuf), "---");

    String status = shortState(state);
    if (fanActive)
        status += " FAN";

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_logisoso16_tr);
    u8g2.setCursor(2, 20);
    u8g2.print(tempBuf);
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.setCursor(2, 36);
    u8g2.print(status);
    u8g2.sendBuffer();
}

void ChamberDisplay::showMessage(const String &line1, const String &line2)
{
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.setCursor(2, 14);
    u8g2.print(line1);
    u8g2.setCursor(2, 30);
    u8g2.print(line2);
    u8g2.sendBuffer();
}
