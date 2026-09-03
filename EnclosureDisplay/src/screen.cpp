#include "screen.h"

TFT_eSPI &tft()
{ // add definition of the free function
    static TFT_eSPI instance;
    return instance;
}

ButtonWidget btnPreheat215 = ButtonWidget(&tft());
ButtonWidget btnPreheat230 = ButtonWidget(&tft());
ButtonWidget btnPreheat250 = ButtonWidget(&tft());
ButtonWidget btnPreheatOff = ButtonWidget(&tft());

// Global Screen instance for button actions
Screen *g_screen = nullptr;

Screen::Screen()
{
    tft().init();
    tft().setRotation(3);
    tft().invertDisplay(1);
    tft().setSwapBytes(true);
    g_screen = this; // Set global instance
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

TFT_eSPI Screen::getTFT()
{
    return tft();
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
    drawTemperaturePanel(5, 129, enclosureTempIcon);

    drawProgressPanel(SCREEN_WIDTH - 160, 50, printProggresIcon);
    drawFilamentPanel(SCREEN_WIDTH - 160, 115);

    btnPreheat215.initButtonUL(BUTTON_SPACING, 190, BUTTON_W, BUTTON_H, TFT_BLACK, TFT_YELLOW, TFT_BLACK, "215", 2);
    btnPreheat215.setPressAction(btnPreheat215_pressAction);
    btnPreheat215.drawSmoothButton(false, 2, TFT_BLACK); // 2 is outline width, TFT_BLACK is the surrounding background colour for anti-aliasing

    btnPreheat230.initButtonUL(BUTTON_SPACING + BUTTON_W + BUTTON_SPACING, 190, BUTTON_W, BUTTON_H, TFT_BLACK, TFT_ORANGE, TFT_BLACK, "230", 2);
    btnPreheat230.setPressAction(btnPreheat230_pressAction);
    btnPreheat230.drawSmoothButton(false, 2, TFT_BLACK);

    btnPreheat250.initButtonUL(2 * (BUTTON_SPACING + BUTTON_W) + BUTTON_SPACING, 190, BUTTON_W, BUTTON_H, TFT_BLACK, TFT_RED, TFT_BLACK, "250", 2);
    btnPreheat250.setPressAction(btnPreheat250_pressAction);
    btnPreheat250.drawSmoothButton(false, 2, TFT_BLACK);

    btnArray[0] = &btnPreheat215;
    btnArray[1] = &btnPreheat230;
    btnArray[2] = &btnPreheat250;

    // Create preheat buttons
    // btnArray[0] = ButtonWidget(tft());
    // btnArray[1] = new ButtonWidget(tft(), 100, 200, BUTTON_W, BUTTON_H, BACKGROUND_PANEL, BACKGROUND, BACKGROUND_PANEL, "230°C", TFT_WHITE, LABEL1_FONT);
    // btnArray[2] = new ButtonWidget(tft(), 190, 200, BUTTON_W, BUTTON_H, BACKGROUND_PANEL, BACKGROUND, BACKGROUND_PANEL, "OFF", TFT_WHITE, LABEL1_FONT);

    // btnArray[0]->setPressAction(btnPreheat215_pressAction);
    // btnArray[1]->setPressAction(btnPreheat230_pressAction);
    // btnArray[2]->setPressAction(btnPreheatOff_pressAction);
}

void btnPreheat215_pressAction(void)
{
    Serial.println("Preheat 215°C pressed");
    if (g_screen != nullptr)
    {
        g_screen->btnPreheat215Pressed();
    }
}
void btnPreheat230_pressAction(void)
{
    Serial.println("Preheat 230°C pressed");
    if (g_screen != nullptr)
    {
        g_screen->btnPreheat230Pressed();
    }
}
void btnPreheat250_pressAction(void)
{
    Serial.println("Preheat 250°C pressed");
    if (g_screen != nullptr)
    {
        g_screen->btnPreheat250Pressed();
    }
}
void btnPreheatOff_pressAction(void)
{
    Serial.println("Preheat OFF pressed");
}

void Screen::btnPreheat215Pressed()
{
    invertButtonFeedback(BUTTON_SPACING, 190, BUTTON_W, BUTTON_H);
}

void Screen::btnPreheat230Pressed()
{
    invertButtonFeedback(BUTTON_SPACING + BUTTON_W + BUTTON_SPACING, 190, BUTTON_W, BUTTON_H);
}

void Screen::btnPreheat250Pressed()
{
    invertButtonFeedback(2 * (BUTTON_SPACING + BUTTON_W) + BUTTON_SPACING, 190, BUTTON_W, BUTTON_H);
}

void Screen::updateTemperature(int x, int y, float actualValue, float prevActualValue, float targetValue, float prevTargetValue)
{
    if (actualValue == prevActualValue && targetValue == prevTargetValue)
    {
        Serial.println("No change in temperature");
        return;
    }

    bool targetValueChanged = (abs((int)targetValue) == 0) != (abs((int)prevTargetValue) == 0);
    bool textGotNarrower = tft().textWidth(String(actualValue, 1)) < tft().textWidth(String(prevActualValue, 1));

    if (targetValueChanged || textGotNarrower)
    {
        tft().fillSmoothRoundRect(x + 40, y - 2, 90, 36, 3, BACKGROUND_PANEL, BACKGROUND_PANEL);
    }
    else
    {
        tft().fillSmoothRoundRect(x + 40, y - 2, 90, 36, 3, BACKGROUND_PANEL, BACKGROUND_PANEL);
        Serial.println("Only value refresh");
    }

    if (targetValue == 0)
    {
        tft().setTextDatum(MR_DATUM);
        tft().fillSmoothRoundRect(x + 38, y - 2, 15, 36, 3, BACKGROUND_PANEL, BACKGROUND_PANEL);
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
    prevTargetValue = targetValue;
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
    updateProgress(SCREEN_WIDTH - 160, 50, printer.progress(), printer.remainingFormatted(), printer.Status());
    updateFilamentPanel(SCREEN_WIDTH - 160, 115, printer.filamentName(), printer.nozzleDiameter());
}

void Screen::updateProgress(int x, int y, float progress, String timePrintLeft, String printerState)
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

    Serial.print("Selected font: ");
    Serial.println(tft().textfont);

    drawProgressBar(2, 175, SCREEN_WIDTH - 10, 12, progress, String(printerState) == "Ready" || String(printerState) == "Operational");

    _prevTimeLeft = timePrintLeft;
}

void Screen::updateFilamentPanel(int x, int y, String filament, String nozzle)
{
    Serial.println("Parsed filament: " + filament);
    Serial.println("Parsed nozzle: " + nozzle);
    if (filament != _prevFilament || nozzle != _prevNozzle)
    {
        tft().fillSmoothRoundRect(x + 33, y, 115, 58, 3, BACKGROUND_PANEL, BACKGROUND);
        tft().setTextDatum(TL_DATUM);
        tft().drawString(nozzle, x + 40, y + 3, FONT_SIZE + 2);
        tft().drawString(filament, x + 40, y + 30, FONT_SIZE + 2);
        _prevFilament = filament;
        _prevNozzle = nozzle;
    }
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
    if (hide)
    {
        tft().fillRect(x, y, width, height, BACKGROUND);
        return;
    }

    // Clamp progress between 0 and 100
    progress = (progress < 0) ? 0 : (progress > 100) ? 100
                                                     : progress;

    // Draw background bar
    tft().drawRect(x, y, width, height, BACKGROUND_PANEL);

    // Calculate filled width
    int filledWidth = (int)((width - 2) * progress / 100.0f);

    // Draw filled portion (green gradient)
    uint16_t barColor = TFT_GREEN;
    if (progress < 75)
        barColor = TFT_YELLOW; // Yellow for < 75%
    if (progress < 50)
        barColor = TFT_RED; // Red for < 50%

    tft().fillRect(x + 1, y + 1, filledWidth, height - 2, barColor);

    // Clear unfilled portion
    if (filledWidth < width - 2)
    {
        tft().fillRect(x + 1 + filledWidth, y + 1, (width - 2) - filledWidth, height - 2, BACKGROUND_PANEL);
    }
}

void Screen::invertButtonFeedback(int x, int y, int w, int h)
{
    if (_invertedButton == nullptr)
    {
        _invertedButton = (ButtonWidget *)1; // Use as a flag (non-null pointer)
        _invertedButtonTime = millis();
        _invertedButtonX = x;
        _invertedButtonY = y;
        _invertedButtonW = w;
        _invertedButtonH = h;
        // Invert only the button rectangle
        tft().invertRect(x, y, w, h);
    }
}

void Screen::updateButtonStates()
{
    if (_invertedButton != nullptr && (millis() - _invertedButtonTime) >= INVERT_DURATION)
    {
        // Invert again to restore original colors
        tft().invertRect(_invertedButtonX, _invertedButtonY, _invertedButtonW, _invertedButtonH);
        _invertedButton = nullptr;
    }
}