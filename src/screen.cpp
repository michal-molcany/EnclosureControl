#include "screen.h"

TFT_eSPI &tft()
{ // add definition of the free function
    static TFT_eSPI instance;
    return instance;
}

Screen::Screen()
{
    tft().init();
    tft().setRotation(1);
    tft().invertDisplay(1);
    tft().setSwapBytes(true);
}

void Screen::ConnectingToWiFi()
{
    tft().fillScreen(TFT_BLACK);
    tft().setTextColor(TFT_WHITE, TFT_BLACK);
    tft().setTextDatum(CC_DATUM);
    tft().drawString("Connecting to WiFi", TFT_HEIGHT / 2, TFT_WIDTH / 2, FONT_SIZE + 2);
    tft().setTextDatum(TL_DATUM);
}
void Screen::ConnectedToWiFi(String ssid)
{
    tft().fillScreen(TFT_BLACK);
    tft().setTextColor(TFT_WHITE, TFT_BLACK);
    tft().drawString("Wifi connected", 10, 30, FONT_SIZE + 2);
    tft().drawString("SSID: " + ssid, 10, 60, FONT_SIZE);
}

void Screen::MainScreen()
{
    currentScreen = MAIN_SCREEN;
    tft().fillScreen(BACKGROUND);
    tft().setCursor(0, 10);
    tft().setTextColor(TFT_WHITE, BACKGROUND);
    tft().drawString("State: ", 5, 5, FONT_SIZE + 2);
    tft().drawLine(0, 35, SCREEN_WIDTH, 35, FONT_COL);
    tft().setTextColor(TFT_WHITE, BACKGROUND_PANEL);

    drawTemperaturePanel(5, 45, extruderTempIcon);
    drawTemperaturePanel(5, 87, bedTempIcon);
    drawTemperaturePanel(5, 129, extruderTempIcon);

    drawProgressPanel(SCREEN_WIDTH - 160, 50, printProggresIcon);
    drawFilamentPanel(SCREEN_WIDTH - 160, 115);
}

void Screen::updateTemperature(int x, int y, float actualValue, float prevActualValue, float targetValue, float prevTargerValue)
{
    if (actualValue == prevActualValue && targetValue == prevTargerValue)
    {
        Serial.println("No change in temperature");
        return;
    }

    if ((targetValue == 0 && prevTargerValue != 0) || (targetValue != 0 && prevTargerValue == 0))
    {
        tft().fillSmoothRoundRect(x + 40, y, 95, 32, 3, BACKGROUND_PANEL, BACKGROUND);
    }

    if (targetValue == 0)
    {
        tft().setTextDatum(MR_DATUM);
        tft().fillSmoothRoundRect(x + 38, y, 15, 32, 3, BACKGROUND_PANEL, BACKGROUND_PANEL);
        tft().drawString(String(actualValue, 1) + "°C", x + 130, y + 18, FONT_SIZE + 2);
    }
    else
    {
        tft().setTextDatum(TR_DATUM);
        tft().drawString(String(actualValue, 1) + "°C", x + 130, y + 0, FONT_SIZE);
        tft().drawString(String(targetValue, 1) + "°C", x + 130, y + 15, FONT_SIZE);
    }

    tft().setTextDatum(TL_DATUM);

    prevActualValue = actualValue;
    prevTargerValue = targetValue;
}

void Screen::updateMainScreen(OctoPrinter printer)
{
    printer.update();

    if (_printerState != printer.Status())
    {
        tft().fillSmoothRoundRect(3, 3, SCREEN_WIDTH - 6, 28, 3, BACKGROUND, BACKGROUND);
        tft().setTextColor(TFT_WHITE, BACKGROUND);
        tft().drawString("State: ", 10, 5, FONT_SIZE + 2);
        tft().drawString(printer.Status(), 85, 5, FONT_SIZE + 2);
        tft().setTextColor(TFT_WHITE, BACKGROUND_PANEL);
        _printerState = printer.Status();
    }

    updateTemperature(5, 45, printer.toolActual(), _printerTool0TempActual, printer.toolTarget(), _printerTool0TempTarget);
    updateTemperature(5, 87, printer.bedActual(), _printerBedTempActual, printer.bedTarget(), _printerBedTempTarget);
    updateTemperature(5, 129, printer.chamberActual(), _printerChamberTempActual, printer.chamberTarget(), _printerChamberTempTarget);
    updateProgress(SCREEN_WIDTH - 160, 50, printer.progress(), printer.remainingFormatted());
}

void Screen::updateProgress(int x, int y, float progress, String timePrintLeft)
{
    if (tft().textWidth(String(_prevProgress, 1) + "%") < tft().textWidth(String(progress, 1) + "%") ||
        tft().textWidth(_prevTimeLeft) < tft().textWidth(timePrintLeft))
    {
        tft().fillSmoothRoundRect(x + 33, y, 115, 58, 3, BACKGROUND_PANEL, BACKGROUND);
    }
    tft().setTextDatum(TR_DATUM);
    tft().drawString(String(progress, 1) + "%", x + 140, y + 3, FONT_SIZE + 2);
    tft().drawString(timePrintLeft, x + 140, y + 30, FONT_SIZE + 2);
    tft().setTextDatum(TL_DATUM);

    _prevTimeLeft = timePrintLeft;
}

void Screen::drawTemperaturePanel(int x, int y, const unsigned short *icon)
{
    tft().fillSmoothRoundRect(x - 2, y - 2, 135, 36, 3, BACKGROUND_PANEL, BACKGROUND);
    tft().pushImage(x, y, 32, 32, icon);
}

void Screen::drawProgressPanel(int x, int y, const unsigned short *icon)
{
    tft().fillSmoothRoundRect(x - 2, y - 2, 150, 60, 3, BACKGROUND_PANEL, BACKGROUND);
    tft().pushImage(x, y + 3, 32, 32, icon);
}

void Screen::drawFilamentPanel(int x, int y)
{
    tft().fillSmoothRoundRect(x - 2, y - 2, 150, 60, 3, BACKGROUND_PANEL, BACKGROUND);
    tft().pushImage(x, y, 32, 32, filamentIcon);
}

void Screen::drawWiFiSignal(int32_t rssi)
{
    // Draw WiFi signal icon in the right upper corner
    tft().fillRect(SCREEN_WIDTH - 33, 3, 27, 27, BACKGROUND); // Clear previous icon
    int bars = 0;
    if (rssi > -55)
        bars = 4;
    else if (rssi > -70)
        bars = 3;
    else if (rssi > -85)
        bars = 2;
    else if (rssi > -100)
        bars = 1;

    for (int i = 0; i < bars; i++)
    {
        tft().fillRect(SCREEN_WIDTH - 25 + (i * 5), 5 + (3 - i) * 5, 4, (i + 1) * 5, TFT_GREEN);
    }
}

void Screen::drawProgressBar(int x, int y, int width, int height, float progress, bool hide)
{
}