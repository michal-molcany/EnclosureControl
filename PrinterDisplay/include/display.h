#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>

#include "const.h"
#include "poller.h"

class Display
{
public:
    using Action = void (*)();

    enum class View
    {
        Main,
        Chamber
    };

    enum class ChamberControl
    {
        Auto,
        Manual
    };

    void Initialization();
    void splashScreen(String message);
    void drawWiFiSignal(int32_t rssi);
    void createLabes();
    void showMainView();
    void showChamberView();
    void chamberSetpointDelta(int delta);
    void louvreDelta(int delta);
    void fanDelta(int delta);
    void setChamberControl(ChamberControl mode);
    ChamberControl chamberControl() const { return _chamberControl; }
    View currentView() const { return _view; }
    void printerStatisticUpdate(const PrinterSnapshot &snap);
    void jobUpdate(const PrinterSnapshot &snap);
    void setPreheatButtonsVisibility(bool visible);
    void setPrintButtonsVisibility(bool visible);
    void setButtonActions(Action preheat215, Action preheat230, Action preheatOff,
                          Action pause, Action stop, Action resume);

private:
    void createPanel(lv_obj_t *parent, lv_obj_t **panel, lv_coord_t x, lv_coord_t y, lv_coord_t width, lv_coord_t height);
    lv_obj_t *createPanelLabel(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y);
    void setButtonVisibility(lv_obj_t **buttons, size_t count, bool visible);
    void createChamberView();
    void refreshChamberDetail();

    lv_display_t *_display = nullptr;
    lv_obj_t *_stateLabel = nullptr;
    lv_obj_t *_toolLabel = nullptr;
    lv_obj_t *_bedLabel = nullptr;
    lv_obj_t *_chamberLabel = nullptr;
    lv_obj_t *_progressLabel = nullptr;
    lv_obj_t *_layerLabel = nullptr;
    lv_obj_t *_timeLabel = nullptr;
    lv_obj_t *_filamentLabel = nullptr;
    lv_obj_t *_nozzleLabel = nullptr;
    lv_obj_t *_lastLayerLabel = nullptr;
    lv_obj_t *_fileNameLabel = nullptr;
    lv_obj_t *_wifiBars[4] = {};
    lv_obj_t *_splashLabel = nullptr;
    lv_obj_t *_splashSpinner = nullptr;
    uint8_t _splashFrame = 0;
    lv_obj_t *_progressBar = nullptr;
    lv_obj_t *_preheatButtons[3] = {};
    lv_obj_t *_printButtons[3] = {};
    View _view = View::Main;
    lv_obj_t *_mainContainer = nullptr;
    lv_obj_t *_chamberContainer = nullptr;
    lv_obj_t *_chamberCurrentLabel = nullptr;
    lv_obj_t *_chamberSetpointLabel = nullptr;
    lv_obj_t *_louvreLabel = nullptr;
    lv_obj_t *_fanLabel = nullptr;
    lv_obj_t *_tempMinusButton = nullptr;
    lv_obj_t *_tempPlusButton = nullptr;
    lv_obj_t *_louvreMinusButton = nullptr;
    lv_obj_t *_louvrePlusButton = nullptr;
    lv_obj_t *_fanMinusButton = nullptr;
    lv_obj_t *_fanPlusButton = nullptr;
    lv_obj_t *_autoButton = nullptr;
    lv_obj_t *_manualButton = nullptr;
    ChamberControl _chamberControl = ChamberControl::Manual;
    int _chamberSetpoint = CHAMBER_SETPOINT_DEFAULT;
    bool _chamberSetpointSeeded = false;
    int _louvreOpen = LOUVRE_DEFAULT;
    int _fanSpeed = FAN_DEFAULT;
    bool _isDrawn = false;
    String _jobFileName;
    String _displayedFileName;
    String _lastRemaining;
    float _progressCompletion = -1;
    int _lastLayerCurrent = -1;
    int _lastLayerTotal = -1;
    String _lastLastDuration;
};

TFT_eSPI &tft();

#endif
