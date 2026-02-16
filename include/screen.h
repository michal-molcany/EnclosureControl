#ifndef SCREEN_H
#define SCREEN_H

#include <TFT_eSPI.h>
#include <TFT_eWidget.h> // Widget library
#include <OctoPrinter.h>

#include "const.h"
#include "icons.h"

class Screen
{
public:
    Screen();
    void ConnectingToWiFi();
    void ConnectedToWiFi(String ssid);
    void drawWiFiSignal(int32_t rssi);
    void updateMainScreen(OctoPrinter printer);

    void MainScreen();
    ButtonWidget *btnArray[9];

    int currentScreen = INITIAL_SCREEN;
    enum ScreenState
    {
        INITIAL_SCREEN,
        MAIN_SCREEN,
        TEMPERATURE_SCREEN,
        FiLES_SCREEN,
    };

private:
    // Add static instance pointer
    static Screen *_instance;

    void drawTemperaturePanel(int x, int y, const unsigned short *icon);
    void drawProgressPanel(int x, int y, const unsigned short *icon);
    void drawProgressBar(int x, int y, int width, int height, float progress, bool hide);
    void drawFilamentPanel(int x, int y);

    void updateTemperature(int x, int y, float actualValue, float prevActualValue, float targetValue, float prevTargerValue);
    void updateProgress(int x, int y, float progress, String timePrintLeft, String printerState);
    void updateFilamentPanel(int x, int y, int filament, String filename);

    String _printerState;
    float _printerTool0TempActual;
    float _printerTool0TempTarget;
    float _printerBedTempActual;
    float _printerBedTempTarget;
    float _printerChamberTempActual;
    float _printerChamberTempTarget;
    float _prevProgress = 0;
    int _prevFilament = -1;
    String _prevTimeLeft = "";

    String _jobFileName = "";
    float _progressCompletion;
    long _jobFileSize;
    long _estimatedPrintTime;
    long _progressPrintTime;
    long _progressPrintTimeLeft;

    String formatTime(long time);
    int parseFilament(String filename);
    String parseNozzle(String filename);
    enum Filament
    {
        PLA,
        PETG,
        ABS,
        ASA,
        TPU,
        PC,
        NYLON,
        HIPS,
        PVA,
        WOOD,
        FLEX,
        UNKNOWN
    };
    String filamentName[12] = {"PLA", "PETG", "ABS", "ASA", "TPU", "PC", "NYLON", "HIPS", "PVA", "WOOD", "FLEX", "UNKN"};
};

TFT_eSPI &tft();

#endif