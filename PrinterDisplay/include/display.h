#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>

#include "const.h"
#include "poller.h"
#include "enclosure_link.h"

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

    enum class ChamberTab
    {
        Material = 0,
        Temperature = 1,
        Manual = 2
    };

    struct MaterialPreset
    {
        const char *name;
        int setpoint;
    };
    static const MaterialPreset MATERIAL_PRESETS[MATERIAL_COUNT];
    static int matchMaterialIndex(const String &filament);
    static uint8_t classifyFilament(const String &filament);

    void Initialization();
    void splashScreen(String message);
    // Remove the splash overlay and restore the background. splashScreen() only
    // adds objects, so after a failed OTA (which is not a reboot) this is what
    // brings the main view back.
    void hideSplash();
    void drawWiFiSignal(int32_t rssi);
    void createLabes();
    void showMainView();
    void showChamberView();
    void chamberSetpointDelta(int delta);
    void louvreDelta(int delta);
    void fanDelta(int delta);
    void setChamberControl(ChamberControl mode);
    void setChamberTab(ChamberTab tab);
    void selectMaterial(int index);
    ChamberControl chamberControl() const { return _chamberControl; }
    ChamberTab chamberTab() const { return _chamberTab; }
    int chamberSetpoint() const { return _chamberSetpoint; }
    int materialIndex() const { return _materialIndex; }
    int louvrePercent() const { return _louvreOpen; }
    int fanPercent() const { return _fanSpeed; }
    // Fill an outbound command from current UI state (magic+seq stamped by link).
    void fillEnclosureCommand(EnclosureCommand &cmd) const;
    // Feed live telemetry from EnclosureFanControl into the chamber view.
    void enclosureUpdate(const EnclosureSnapshot &snap);
    View currentView() const { return _view; }
    void printerStatisticUpdate(const PrinterSnapshot &snap);
    void jobUpdate(const PrinterSnapshot &snap);
    void setPreheatButtonsVisibility(bool visible);
    void setPrintButtonsVisibility(bool visible);
    void setJobActive(bool active);
    void setButtonActions(Action preheat215, Action preheat230, Action preheatOff,
                           Action pause, Action stop, Action resume);

private:
    void createPanel(lv_obj_t *parent, lv_obj_t **panel, lv_coord_t x, lv_coord_t y, lv_coord_t width, lv_coord_t height);
    lv_obj_t *createPanelLabel(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y);
    void setButtonVisibility(lv_obj_t **buttons, size_t count, bool visible);
    void createChamberView();
    void refreshChamberDetail();
    void refreshEnclosureLinkHeader();
    void applyMainJobLayout(bool active);
    void clearJobDisplay();
    void pushEnclosureCommand();
    // Auto-switch Material profile from OctoPrint filament (Material tab only).
    void autoSelectMaterial(const String &filament, bool jobActive);
    static String normalizeFilament(const String &raw);
    static bool filamentContains(const String &norm, const char *token);

    lv_display_t *_display = nullptr;
    lv_obj_t *_stateLabel = nullptr;
    lv_obj_t *_toolPanel = nullptr;
    lv_obj_t *_bedPanel = nullptr;
    lv_obj_t *_chamberPanel = nullptr;
    lv_obj_t *_progressPanel = nullptr;
    lv_obj_t *_filamentPanel = nullptr;
    lv_obj_t *_filePanel = nullptr;
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
    lv_obj_t *_encLinkIcon = nullptr;
    bool _encLinkUp = false;
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
    lv_obj_t *_chamberTabview = nullptr;
    lv_obj_t *_matContainer = nullptr;
    lv_obj_t *_tempContainer = nullptr;
    lv_obj_t *_manContainer = nullptr;
    lv_obj_t *_materialButtons[MATERIAL_COUNT] = {};
    lv_obj_t *_tempInfoLabel = nullptr;
    lv_obj_t *_tempStateLabel = nullptr;
    ChamberControl _chamberControl = ChamberControl::Auto;
    ChamberTab _chamberTab = ChamberTab::Material;
    int _chamberSetpoint = CHAMBER_SETPOINT_DEFAULT;
    bool _chamberSetpointSeeded = false;
    int _materialIndex = MATERIAL_DEFAULT_INDEX;
    uint8_t _commandMaterial = ENC_MATERIAL_PLA;
    int _louvreOpen = LOUVRE_DEFAULT;
    int _fanSpeed = FAN_DEFAULT;
    String _octoFilament;
    bool _wasJobActive = false;
    String _lastAutoFilament;
    EnclosureSnapshot _enclosure; // last telemetry from EnclosureFanControl
    bool _isDrawn = false;
    bool _jobLayoutInitialized = false;
    bool _jobLayoutActive = false;
    bool _jobActive = false;
    bool _enclosureCommandDirty = false;
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
