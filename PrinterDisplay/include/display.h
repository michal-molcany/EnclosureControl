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
        Chamber,
        Tool,
        Bed,
        Files,
        FileInfo
    };

    // Targets that get their own preheat screen, reached by tapping the
    // matching panel on the main view. Index PreheatKind::Count maps directly
    // onto the internal per-target state array.
    enum class PreheatKind
    {
        Tool = 0,
        Bed = 1,
        Count = 2
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

    // Target presets, indexed in the same order as MATERIAL_PRESETS.
    static const int TOOL_PRESETS[MATERIAL_COUNT];
    static const int BED_PRESETS[MATERIAL_COUNT];

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
    void showToolView();
    void showBedView();
    // Printable-files view, reached by tapping the idle "No active print"
    // panel. Shows the cached list only; use openFilesView() for the tap
    // entry, which also asks the poll task for a fresh fetch.
    void showFilesView();
    // Shows the selected file's details (material, print time, size, upload date,
    // dimensions, filament, print history). Reads the same listing the rows were
    // built from, so it is immediate and costs no request.
    void showFileInfo();
    // A touch anywhere on the details panel. Returns to the list, but ONLY while
    // the panel is showing - see the note on the implementation, which explains
    // why that guard is load-bearing rather than tidy.
    void filesPanelTouched();
    void openFilesView();
    // Send a target to OctoPrint. Inert while a print is active.
    void setPreheatTarget(PreheatKind kind, int temp);
    void selectPreheatPreset(PreheatKind kind, int index);
    void stepPreheatTarget(PreheatKind kind, int delta);
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
    // Job state from the OctoPrint snapshot: active = printing || paused.
    // Pause shows only while printing, Resume only while paused, Cancel
    // whenever a job is up. The two visible buttons fill the bottom row.
    void setJobState(bool active, bool paused);
    // Only the three job buttons live on the main view. Tool preheats go straight
    // to the poll task from main.cpp; bed targets are set through the bed view.
    void setButtonActions(Action pause, Action stop, Action resume);
    // Pause/Stop go through a Yes/Cancel confirm dialog; Resume stays immediate.
    // jobCommand is a JobCommand value (PAUSE_JOB or STOP_JOB).
    void showJobConfirm(int jobCommand);
    void hideJobConfirm();
    void confirmPendingJob();
    bool isJobConfirmOpen() const { return _pendingJob >= 0; }
    // Newest-first file list from the poller cache (name + server date per
    // row). Called from the UI loop like jobUpdate(); rebuilds rows only when
    // the cache version changes.
    void filesUpdate();
    // Selects a cached row (0-based): highlights it and reveals the Print
    // button. -1 clears the selection.
    void selectFile(int index);
    // Print confirm for the selected row, reusing the job confirm overlay
    // ("Print <name>?"). Yes stages the path to the poll task.
    void showPrintConfirm();
    // The files view sends through these. Open asks the poll task for a fresh
    // list; print carries the selected OctoPrint path (folders included).
    using FilePrintAction = void (*)(const char *path);
    void setFilesActions(Action open, FilePrintAction print);
    // Main-view preheat buttons: PLA / PETG / ASA / off, each setting nozzle + bed.
    void setToolActions(Action pla, Action petg, Action asa, Action off);
    // The preheat views send through this. It takes the temperature rather than
    // being one of a fixed set, because the stepper produces open-ended values.
    using PreheatAction = void (*)(int temp);
    void setPreheatAction(PreheatKind kind, PreheatAction action);

private:
    // One preheat screen's worth of widgets plus the state that drives it. The
    // nozzle and bed screens are the same UI over different targets, so they
    // share this type and a single set of helpers rather than duplicating them.
    struct PreheatView
    {
        lv_obj_t *container = nullptr;
        lv_obj_t *presetButtons[MATERIAL_COUNT] = {};
        lv_obj_t *offButton = nullptr;
        lv_obj_t *minusButton = nullptr;
        lv_obj_t *plusButton = nullptr;
        lv_obj_t *currentLabel = nullptr;
        lv_obj_t *targetLabel = nullptr;
        // Current state, mirrored so the screen survives leaving and returning.
        int target = 0;
        uint32_t sentMs = 0;
        double actual = 0;
        bool actualValid = false;
        bool controlsEnabled = true;
    };

    static constexpr size_t kPreheatCount = static_cast<size_t>(PreheatKind::Count);
    static int preheatMax(PreheatKind kind);
    static int preheatMin(PreheatKind kind);
    static const int *preheatPresets(PreheatKind kind);
    static View preheatView(PreheatKind kind);

    void createPanel(lv_obj_t *parent, lv_obj_t **panel, lv_coord_t x, lv_coord_t y, lv_coord_t width, lv_coord_t height);
    lv_obj_t *createPanelLabel(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y);
    void setButtonVisibility(lv_obj_t **buttons, size_t count, bool visible);
    void createChamberView();
    void createPreheatView(PreheatKind kind, const char *title);
    void createFilesView();
    void showPreheatView(PreheatKind kind);
    void createJobConfirm();
    void refreshPreheatDetail(PreheatKind kind);
    // Rebuilds the file rows from _files when the cache version moved, and
    // owns the status line (Loading / count / empty / error) plus the Print
    // button visibility. Row buttons are created once and recycled.
    void refreshFilesDetail();
    // Grey out and unclick a preheat screen's controls while a print runs.
    void setPreheatControlsEnabled(PreheatKind kind, bool enabled);
    void refreshChamberDetail();
    void refreshEnclosureLinkHeader();
    void applyMainJobLayout(bool active);
    void clearJobDisplay();
    // Show/hide + reflow the Pause/Cancel/Resume row for _jobActive/_jobPaused.
    void refreshJobButtons();
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
    lv_obj_t *_bedWavesIcon = nullptr;
    uint8_t _bedWavesBand = 255;
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
    // Header icon for the HTTP enclosure-API fallback link. Shown only while the
    // API is the transport carrying C3 telemetry (fromApi + fresh); hidden the
    // rest of the time. Distinct glyph from the C3 chip so the two read apart.
    lv_obj_t *_apiLinkIcon = nullptr;
    bool _apiLinkUp = false;
    lv_obj_t *_splashLabel = nullptr;
    lv_obj_t *_splashSpinner = nullptr;
    uint8_t _splashFrame = 0;
    lv_obj_t *_progressBar = nullptr;
    lv_obj_t *_preheatButtons[4] = {};
    lv_obj_t *_printButtons[3] = {};
    lv_obj_t *_confirmOverlay = nullptr;
    lv_obj_t *_confirmBox = nullptr;
    lv_obj_t *_confirmLabel = nullptr;
    lv_obj_t *_confirmSubLabel = nullptr;
    lv_obj_t *_confirmYesButton = nullptr;
    lv_obj_t *_confirmCancelButton = nullptr;
    int _pendingJob = -1;
    View _view = View::Main;
    lv_obj_t *_mainContainer = nullptr;
    lv_obj_t *_chamberContainer = nullptr;
    PreheatView _preheat[kPreheatCount];
    PreheatAction _preheatAction[kPreheatCount] = {};
    // Printable-files view widgets. Rows are a fixed pool of clickable
    // labels (one LVGL object per row - a button + label per row exhausted
    // the 60 KB pool and crashed createJobConfirm), created once in
    // createFilesView() and recycled by refreshFilesDetail(), so opening the
    // list never mallocs. Row text lives in the labels; the list itself stays
    // on the poll task side and is pulled entry-by-entry (no multi-KB mirror:
    // static DRAM is ~800 bytes under the link limit).
    lv_obj_t *_filesContainer = nullptr;
    lv_obj_t *_filesList = nullptr;
    lv_obj_t *_filesStatusLabel = nullptr;
    lv_obj_t *_filesPrintButton = nullptr;
    lv_obj_t *_filesInfoButton = nullptr;
    // File details as a real two-column table. The name sits on its own line
    // above it, because it is what the table is about and squeezing a long
    // filename into a cell makes both columns unreadable.
    lv_obj_t *_filesInfoTable = nullptr;
    lv_obj_t *_fileRowLabels[MAX_PRINT_FILES] = {};
    uint8_t _filesCount = 0;
    uint32_t _filesVersionSeen = 0;
    bool _filesBusy = false;
    bool _filesError = false;
    int _selectedFile = -1;
    Action _filesOpenAction = nullptr;
    FilePrintAction _filePrintAction = nullptr;
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
    bool _jobPaused = false;
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
