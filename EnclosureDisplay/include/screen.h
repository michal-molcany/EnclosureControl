#ifndef SCREEN_H
#define SCREEN_H

#include <TFT_eSPI.h>
#include <TFT_eWidget.h> // Widget library
#include <OctoPrinter.h>

#include "const.h"
#include "icons.h"

// Forward declarations for button press actions
void btnPreheat215_pressAction(void);
void btnPreheat230_pressAction(void);
void btnPreheat250_pressAction(void);
void btnPreheatOff_pressAction(void);

class Screen
{
public:
    Screen();
    void ConnectingToWiFi();
    void ConnectedToWiFi(String ssid);
    void drawWiFiSignal(int32_t rssi);
    void updateMainScreen(OctoPrinter printer);

    void MainScreen();

    TFT_eSPI getTFT();
    ButtonWidget *btnArray[3];

    int currentScreen = INITIAL_SCREEN;
    enum ScreenState
    {
        INITIAL_SCREEN,
        MAIN_SCREEN,
        TEMPERATURE_SCREEN,
        FiLES_SCREEN,
    };

    // Button press handlers
    void btnPreheat215Pressed();
    void btnPreheat230Pressed();
    void btnPreheat250Pressed();

private:
    // Add static instance pointer
    static Screen *_instance;

    void drawTemperaturePanel(int x, int y, const unsigned short *icon);
    void drawProgressPanel(int x, int y, const unsigned short *icon);
    void drawProgressBar(int x, int y, int width, int height, float progress, bool hide);
    void drawFilamentPanel(int x, int y);

    void updateTemperature(int x, int y, float actualValue, float prevActualValue, float targetValue, float prevTargerValue);
    void updateProgress(int x, int y, float progress, String timePrintLeft, String printerState);
    void updateFilamentPanel(int x, int y, String filament, String nozzle);

    void invertButtonFeedback(int x, int y, int w, int h);
    void updateButtonStates();

    String _printerState;
    float _printerTool0TempActual;
    float _printerTool0TempTarget;
    float _printerBedTempActual;
    float _printerBedTempTarget;
    float _printerChamberTempActual;
    float _printerChamberTempTarget;
    float _prevProgress = 0;
    String _prevFilament = "";
    String _prevTimeLeft = "";

    String _jobFileName = "";
    float _progressCompletion;
    long _jobFileSize;
    long _estimatedPrintTime;
    long _progressPrintTime;
    long _progressPrintTimeLeft;
    String _prevNozzle = "";

    String formatTime(long time);

    // Button inversion feedback tracking
    ButtonWidget *_invertedButton = nullptr;
    unsigned long _invertedButtonTime = 0;
    const unsigned long INVERT_DURATION = 200; // milliseconds
    int _invertedButtonX = 0;
    int _invertedButtonY = 0;
    int _invertedButtonW = 0;
    int _invertedButtonH = 0;
    // enum Filament
    // {
    //     PLA,
    //     PETG,
    //     ABS,
    //     ASA,
    //     TPU,
    //     PC,
    //     NYLON,
    //     HIPS,
    //     PVA,
    //     WOOD,
    //     FLEX,
    //     UNKNOWN
    // };
    // String filamentName[12] = {"PLA", "PETG", "ABS", "ASA", "TPU", "PC", "NYLON", "HIPS", "PVA", "WOOD", "FLEX", "UNKN"};

    // enum Nozzle
    // {
    //     NOZZLE_0_25,
    //     NOZZLE_0_4,
    //     NOZZLE_0_6,
    //     NOZZLE_0_8,
    //     NOZZLE_1_0,
    //     UNKNOWN_NOZZLE
    // };
    // String nozzleName[6] = {"0.25", "0.4", "0.6", "0.8", "1.0", "UNKN"};
};

TFT_eSPI &tft();

#endif