#include "display.h"

#include <XPT2046_Touchscreen.h>

#include "enclosure_link.h"

#include "icons/icon_bed.h"
#include "icons/icon_c3.h"
#include "icons/icon_chamber.h"
#include "icons/icon_fan.h"
#include "icons/icon_filament.h"
#include "icons/icon_louvre.h"
#include "icons/icon_nozzle.h"
#include "icons/icon_nozzle_size.h"
#include "icons/icon_progress.h"

const Display::MaterialPreset Display::MATERIAL_PRESETS[MATERIAL_COUNT] = {
    {"PLA", MATERIAL_PLA_SETPOINT},
    {"PETG", MATERIAL_PETG_SETPOINT},
    {"ASA", MATERIAL_ASA_SETPOINT},
    {"ABS", MATERIAL_ABS_SETPOINT},
    {"TPU", MATERIAL_TPU_SETPOINT},
    {"Nylon", MATERIAL_NYLON_SETPOINT},
};

int Display::matchMaterialIndex(const String &filament)
{
    const String norm = normalizeFilament(filament);
    if (norm.length() == 0)
        return -1;
    if (filamentContains(norm, "PLA"))
        return 0;
    if (filamentContains(norm, "PETG"))
        return 1;
    if (filamentContains(norm, "ASA"))
        return 2;
    if (filamentContains(norm, "ABS"))
        return 3;
    if (filamentContains(norm, "TPU"))
        return 4;
    if (filamentContains(norm, "NYLON") || filamentContains(norm, "POLYAMIDE") ||
        filamentContains(norm, "PA"))
        return 5;
    return -1;
}

uint8_t Display::classifyFilament(const String &filament)
{
    const int matched = matchMaterialIndex(filament);
    if (matched >= 0)
        return (uint8_t)matched;

    const String norm = normalizeFilament(filament);
    static const char *highTempMaterials[] = {
        "PC", "POLYCARBONATE", "HIPS", "PVA", "WOOD", "FLEX", "PP", "PPSU", "PPS", "PEI", "ULTEM"
    };
    for (const char *candidate : highTempMaterials)
    {
        if (filamentContains(norm, candidate))
            return ENC_MATERIAL_HIGH_TEMP;
    }
    return ENC_MATERIAL_UNKNOWN;
}

String Display::normalizeFilament(const String &raw)
{
    String s = raw;
    s.trim();
    s.toUpperCase();
    return s;
}

bool Display::filamentContains(const String &norm, const char *token)
{
    if (norm.length() == 0 || token == nullptr)
        return false;

    const int tokenLength = strlen(token);
    if (tokenLength == 0)
        return false;

    auto isLetter = [](char value) {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
    };

    for (int start = norm.indexOf(token); start >= 0; start = norm.indexOf(token, start + 1))
    {
        const int end = start + tokenLength;
        const bool leftBoundary = start == 0 || !isLetter(norm[start - 1]);
        const bool rightBoundary = end >= norm.length() || !isLetter(norm[end]);
        if (leftBoundary && rightBoundary)
            return true;
    }
    return false;
}

namespace
{
    TFT_eSPI displayTft;    SPIClass touchSPI(HSPI);
    XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);
    lv_color_t drawBuffer[320 * 10];
    Display::Action buttonActions[6] = {};
    Display *activeDisplay = nullptr;

    lv_color_t color565(uint16_t color)
    {
        const uint8_t red = ((color >> 11) & 0x1F) * 255 / 31;
        const uint8_t green = ((color >> 5) & 0x3F) * 255 / 63;
        const uint8_t blue = (color & 0x1F) * 255 / 31;
        return lv_color_make(red, green, blue);
    }

    lv_color_t pressed565(uint16_t color)
    {
        // Darken ~40% for LV_STATE_PRESSED feedback.
        const uint8_t red = ((color >> 11) & 0x1F) * 255 / 31 * 3 / 5;
        const uint8_t green = ((color >> 5) & 0x3F) * 255 / 63 * 3 / 5;
        const uint8_t blue = (color & 0x1F) * 255 / 31 * 3 / 5;
        return lv_color_make(red, green, blue);
    }

    void styleButton(lv_obj_t *button, uint16_t color)
    {
        lv_obj_set_style_bg_color(button, color565(color), 0);
        lv_obj_set_style_bg_color(button, pressed565(color), LV_STATE_PRESSED);
    }

    // Set label text only when it actually changed. Every lv_label_set_text
    // frees + re-mallocs the buffer; called at 4 Hz on hidden labels this
    // fragments the heap until an allocation fails (label->text stays NULL
    // and the next layout pass panics with LoadProhibited).
    void setLabelTextOnce(lv_obj_t *label, const String &text)
    {
        if (!label)
            return;
        const char *current = lv_label_get_text(label);
        if (current && text.equals(current))
            return;
        lv_label_set_text(label, text.c_str());
    }

    void flushDisplay(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
    {
        const uint32_t width = area->x2 - area->x1 + 1;
        const uint32_t height = area->y2 - area->y1 + 1;
        displayTft.startWrite();
        displayTft.setAddrWindow(area->x1, area->y1, width, height);
        displayTft.pushColors(reinterpret_cast<uint16_t *>(pixels), width * height, true);
        displayTft.endWrite();
        lv_display_flush_ready(display);
    }

    void readTouch(lv_indev_t *, lv_indev_data_t *data)
    {
        // Poll pressure directly: the IRQ latch (tirqTouched) can stick
        // false on some CYD units and would permanently gate touch.
        if (!touchscreen.touched())
        {
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        TS_Point point = touchscreen.getPoint();
#if SERIAL_DEBUG
        static uint32_t lastLog = 0;
        if (millis() - lastLog > 500)
        {
            lastLog = millis();
            Serial.printf("touch raw x=%d y=%d z=%d -> lv x=%d y=%d\n",
                          point.x, point.y, point.z,
                          map(point.x, 200, 3700, 1, SCREEN_WIDTH),
                          map(point.y, 240, 3800, 1, SCREEN_HEIGHT));
        }
#endif
        data->point.x = map(point.x, 200, 3700, 1, SCREEN_WIDTH);
        data->point.y = map(point.y, 240, 3800, 1, SCREEN_HEIGHT);
        data->state = LV_INDEV_STATE_PRESSED;
    }

    void buttonEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) == LV_EVENT_CLICKED)
        {
            const uintptr_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
            if (index < 6 && buttonActions[index])
                buttonActions[index]();
        }
    }

    void chamberPanelEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) == LV_EVENT_CLICKED && activeDisplay)
            activeDisplay->showChamberView();
    }

    void backEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) == LV_EVENT_CLICKED && activeDisplay)
            activeDisplay->showMainView();
    }

    void stepperEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) != LV_EVENT_CLICKED || !activeDisplay)
            return;
        const intptr_t delta = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
        activeDisplay->chamberSetpointDelta(static_cast<int>(delta));
    }

    void louvreStepperEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) != LV_EVENT_CLICKED || !activeDisplay)
            return;
        const intptr_t delta = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
        activeDisplay->louvreDelta(static_cast<int>(delta));
    }

    void fanStepperEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) != LV_EVENT_CLICKED || !activeDisplay)
            return;
        const intptr_t delta = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
        activeDisplay->fanDelta(static_cast<int>(delta));
    }

    void chamberTabEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED || !activeDisplay)
            return;
        lv_obj_t *tabview = static_cast<lv_obj_t *>(lv_event_get_current_target(event));
        const uint32_t tab = lv_tabview_get_tab_active(tabview);
        if (tab <= 2)
            activeDisplay->setChamberTab(static_cast<Display::ChamberTab>(tab));
    }

    void materialButtonEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) != LV_EVENT_CLICKED || !activeDisplay)
            return;
        const intptr_t index = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
        activeDisplay->selectMaterial(static_cast<int>(index));
    }

    lv_obj_t *createChamberStepperButton(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                        const char *text, uint16_t color,
                                        lv_event_cb_t handler, int delta)
    {
        lv_obj_t *button = lv_button_create(parent);
        lv_obj_set_size(button, 60, 36);
        lv_obj_set_pos(button, x, y);
        styleButton(button, color);
        lv_obj_add_event_cb(button, handler, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(delta)));
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(label);
        return button;
    }

    void placeRowIcon(lv_obj_t *parent, const lv_image_dsc_t *icon, lv_coord_t y)
    {
        lv_obj_t *image = lv_image_create(parent);
        lv_image_set_src(image, icon);
        lv_obj_set_style_bg_opa(image, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(image, 0, 0);
        lv_obj_set_pos(image, 8, y);
    }

    void styleLabel(lv_obj_t *label)
    {
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    }

    void updateTempLabel(lv_obj_t *label, double actual, int target)
    {
        if (target == 0)
        {
            setLabelTextOnce(label, String(actual, 1) + " C");
            lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
        }
        else
        {
            setLabelTextOnce(label, String(actual, 1) + " / " + String(target) + " C");
            lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        }
    }

    String formatHMS(int totalSeconds)
    {
        if (totalSeconds < 0)
            totalSeconds = 0;
        const int hours = totalSeconds / 3600;
        const int minutes = (totalSeconds % 3600) / 60;
        const int seconds = totalSeconds % 60;
        String result;
        if (hours > 0)
        {
            result += String(hours);
            result += ":";
            if (minutes < 10)
                result += "0";
        }
        result += String(minutes);
        result += ":";
        if (seconds < 10)
            result += "0";
        result += String(seconds);
        return result;
    }

    String trimDuration(const String &value)
    {
        String result = value;
        result.trim();
        // DisplayLayerProgress "0h:01m:12s" style -> compact "1:12" / "1:02:03".
        int hours = 0, minutes = 0, seconds = 0;
        if (sscanf(result.c_str(), "%dh:%dm:%ds", &hours, &minutes, &seconds) == 3)
            return formatHMS(hours * 3600 + minutes * 60 + seconds);
        while (result.startsWith("00:"))
            result.remove(0, 3);
        if (result.length() > 1 && result[0] == '0' && isDigit(result[1]))
            result.remove(0, 1);
        return result;
    }
}

TFT_eSPI &tft()
{
    return displayTft;
}

void Display::Initialization()
{
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
    displayTft.begin();
    displayTft.setRotation(1);
    displayTft.invertDisplay(false);
    displayTft.setSwapBytes(true);
    displayTft.fillScreen(TFT_BLACK);
    // CYD touch is on its own pins (CLK 25 / MISO 39 / MOSI 32 / CS 33);
    // the XPT2046 default of SPI.begin() uses 18/19/23 and never sees touches.
    touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    touchscreen.begin(touchSPI);
    touchscreen.setRotation(1);

    lv_init();
    _display = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_display_set_color_format(_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(_display, flushDisplay);
    lv_display_set_buffers(_display, drawBuffer, nullptr, sizeof(drawBuffer), LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *inputDevice = lv_indev_create();
    lv_indev_set_type(inputDevice, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(inputDevice, readTouch);
    lv_indev_set_display(inputDevice, _display);

    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x000000), 0);
}

void Display::splashScreen(String message)
{
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x000000), 0);
    if (!_splashSpinner)
    {
        _splashSpinner = lv_arc_create(lv_screen_active());
        lv_obj_set_size(_splashSpinner, 80, 80);
        lv_obj_align(_splashSpinner, LV_ALIGN_CENTER, 0, -40);
        lv_arc_set_bg_angles(_splashSpinner, 0, 360);
        lv_arc_set_angles(_splashSpinner, 0, 90);
        lv_arc_set_rotation(_splashSpinner, 270);
        lv_obj_set_style_arc_color(_splashSpinner, lv_color_hex(0x333333), LV_PART_MAIN);
        lv_obj_set_style_arc_color(_splashSpinner, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(_splashSpinner, 8, LV_PART_MAIN);
        lv_obj_set_style_arc_width(_splashSpinner, 8, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(_splashSpinner, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_remove_flag(_splashSpinner, LV_OBJ_FLAG_CLICKABLE);
    }
    if (!_splashLabel)
    {
        _splashLabel = lv_label_create(lv_screen_active());
        styleLabel(_splashLabel);
        lv_obj_align(_splashLabel, LV_ALIGN_CENTER, 0, 35);
    }
    _splashFrame = (_splashFrame + 1) % 12;
    lv_arc_set_rotation(_splashSpinner, 270 + _splashFrame * 30);
    lv_label_set_text(_splashLabel, message.c_str());
    lv_refr_now(_display);
}

void Display::createPanel(lv_obj_t *parent, lv_obj_t **panel, lv_coord_t x, lv_coord_t y, lv_coord_t width, lv_coord_t height)
{
    *panel = lv_obj_create(parent ? parent : lv_screen_active());
    lv_obj_set_pos(*panel, x, y);
    lv_obj_set_size(*panel, width, height);
    lv_obj_set_style_bg_color(*panel, color565(BACKGROUND_COL), 0);
    lv_obj_set_style_border_color(*panel, lv_color_hex(0x5A5A5A), 0);
    lv_obj_set_style_border_width(*panel, 1, 0);
    lv_obj_set_style_radius(*panel, 3, 0);
    lv_obj_set_style_pad_all(*panel, 4, 0);
    lv_obj_clear_flag(*panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(*panel, LV_SCROLLBAR_MODE_OFF);
}

lv_obj_t *Display::createPanelLabel(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    styleLabel(label);
    return label;
}

static lv_obj_t *createPanelIcon(lv_obj_t *parent, const lv_image_dsc_t *icon, lv_align_t align = LV_ALIGN_LEFT_MID)
{
    lv_obj_t *image = lv_image_create(parent);
    lv_image_set_src(image, icon);
    lv_obj_set_style_bg_opa(image, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(image, 0, 0);
    lv_obj_align(image, align, align == LV_ALIGN_RIGHT_MID ? -4 : 4, 0);
    return image;
}



static lv_obj_t *createViewContainer(bool hidden)
{
    lv_obj_t *container = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_size(container, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(container, color565(BACKGROUND), 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_OFF);
    if (hidden)
        lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
    return container;
}

void Display::createLabes()
{
    if (_isDrawn)
        return;
    activeDisplay = this;
    lv_obj_clean(lv_screen_active());
    _splashLabel = nullptr;
    _splashSpinner = nullptr;
    _splashFrame = 0;
    lv_obj_set_style_bg_color(lv_screen_active(), color565(BACKGROUND), 0);

    _mainContainer = createViewContainer(false);
    _chamberContainer = createViewContainer(true);

    _stateLabel = lv_label_create(_mainContainer);
    lv_label_set_text(_stateLabel, "State: ");
    lv_obj_set_pos(_stateLabel, 10, 5);
    styleLabel(_stateLabel);

    const lv_coord_t barHeights[] = {4, 7, 10, 13};
    for (size_t index = 0; index < 4; ++index)
    {
        _wifiBars[index] = lv_obj_create(_mainContainer);
        lv_obj_set_size(_wifiBars[index], 7, barHeights[index]);
        lv_obj_set_pos(_wifiBars[index], 275 + index * 11, 22 - barHeights[index]);
        lv_obj_set_style_radius(_wifiBars[index], 1, 0);
        lv_obj_set_style_border_width(_wifiBars[index], 0, 0);
        lv_obj_set_style_bg_color(_wifiBars[index], color565(TFT_GREEN), 0);
        lv_obj_add_flag(_wifiBars[index], LV_OBJ_FLAG_HIDDEN);
    }

    _encLinkIcon = lv_image_create(_mainContainer);
    lv_image_set_src(_encLinkIcon, &icon_c3);
    lv_obj_set_size(_encLinkIcon, 16, 16);
    lv_obj_set_pos(_encLinkIcon, 252, 6);
    lv_obj_set_style_image_recolor(_encLinkIcon, color565(0x808080), 0);
    lv_obj_set_style_image_recolor_opa(_encLinkIcon, LV_OPA_COVER, 0);
    _encLinkUp = false;

    lv_obj_t *separator = lv_obj_create(_mainContainer);
    lv_obj_set_pos(separator, 0, 28);
    lv_obj_set_size(separator, SCREEN_WIDTH, 1);
    lv_obj_set_style_bg_color(separator, color565(FONT_COL), 0);
    lv_obj_set_style_border_width(separator, 0, 0);

    createPanel(_mainContainer, &_toolPanel, 5, 35, 130, 36);
    createPanel(_mainContainer, &_bedPanel, 5, 76, 130, 36);
    createPanel(_mainContainer, &_chamberPanel, 5, 117, 130, 36);
    createPanel(_mainContainer, &_progressPanel, 140, 35, 175, 48);
    createPanel(_mainContainer, &_filamentPanel, 140, 88, 175, 30);
    createPanel(_mainContainer, &_filePanel, 140, 123, 175, 30);
    lv_obj_add_flag(_chamberPanel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_chamberPanel, chamberPanelEvent, LV_EVENT_CLICKED, nullptr);

    createPanelIcon(_toolPanel, &icon_nozzle);
    createPanelIcon(_bedPanel, &icon_bed);
    createPanelIcon(_chamberPanel, &icon_chamber);
    createPanelIcon(_progressPanel, &icon_progress);
    createPanelIcon(_filamentPanel, &icon_filament);
    lv_obj_align(createPanelIcon(_filamentPanel, &icon_nozzle_size), LV_ALIGN_LEFT_MID, 82, 0);
    _toolLabel = createPanelLabel(_toolPanel, "-- / -- C", 50, 6);
    _bedLabel = createPanelLabel(_bedPanel, "-- / -- C", 50, 6);
    _chamberLabel = createPanelLabel(_chamberPanel, "-- C", 60, 6);
    lv_obj_align(_toolLabel, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_align(_bedLabel, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_align(_chamberLabel, LV_ALIGN_RIGHT_MID, -4, 0);
    _lastLayerLabel = createPanelLabel(_progressPanel, "L --", 32, 2);
    _timeLabel = createPanelLabel(_progressPanel, "--", 0, 2);
    lv_obj_align(_timeLabel, LV_ALIGN_TOP_RIGHT, -4, 2);
    _progressLabel = createPanelLabel(_progressPanel, "-- %", 32, 22);
    lv_label_set_long_mode(_progressLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_progressLabel, 70);
    _layerLabel = createPanelLabel(_progressPanel, "L--/--", 0, 22);
    lv_obj_align(_layerLabel, LV_ALIGN_TOP_RIGHT, -4, 22);
    _filamentLabel = createPanelLabel(_filamentPanel, "--", 32, 0);
    lv_obj_align(_filamentLabel, LV_ALIGN_LEFT_MID, 32, 0);
    lv_label_set_long_mode(_filamentLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_filamentLabel, 46);
    _nozzleLabel = createPanelLabel(_filamentPanel, "--", 110, 0);
    lv_obj_align(_nozzleLabel, LV_ALIGN_LEFT_MID, 110, 0);
    lv_label_set_long_mode(_nozzleLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_nozzleLabel, 53);
    _fileNameLabel = createPanelLabel(_filePanel, "--", 4, 0);
    lv_obj_align(_fileNameLabel, LV_ALIGN_LEFT_MID, 4, 0);
    lv_label_set_long_mode(_fileNameLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(_fileNameLabel, 159);

    _progressBar = lv_bar_create(_mainContainer);
    lv_obj_set_pos(_progressBar, 2, 160);
    lv_obj_set_size(_progressBar, SCREEN_WIDTH - 10, 10);
    lv_bar_set_range(_progressBar, 0, 100);
    lv_obj_set_style_bg_color(_progressBar, color565(BACKGROUND), LV_PART_MAIN);
    lv_obj_set_style_border_color(_progressBar, color565(BACKGROUND_COL), LV_PART_MAIN);
    lv_obj_set_style_border_width(_progressBar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(_progressBar, color565(TFT_GREEN), LV_PART_INDICATOR);

    const char *preheatLabels[] = {"215C", "230C", "Off"};
    const uint32_t preheatColors[] = {TFT_ORANGE, TFT_RED, TFT_BLUE};
    for (size_t index = 0; index < 3; ++index)
    {
        _preheatButtons[index] = lv_button_create(_mainContainer);
        lv_obj_set_size(_preheatButtons[index], 80, 40);
        lv_obj_set_pos(_preheatButtons[index], 10 + index * 90, 180);
        styleButton(_preheatButtons[index], preheatColors[index]);
        lv_obj_add_event_cb(_preheatButtons[index], buttonEvent, LV_EVENT_CLICKED, reinterpret_cast<void *>(index));
        lv_obj_t *label = lv_label_create(_preheatButtons[index]);
        lv_label_set_text(label, preheatLabels[index]);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(label);
    }

    const char *printLabels[] = {"Pause", "Stop", "Resume"};
    const uint32_t printColors[] = {TFT_ORANGE, TFT_RED, TFT_DARKGREEN};
    const lv_coord_t printWidths[] = {90, 100, 110};
    const lv_coord_t printX[] = {5, 100, 205};
    for (size_t index = 0; index < 3; ++index)
    {
        _printButtons[index] = lv_button_create(_mainContainer);
        lv_obj_set_size(_printButtons[index], printWidths[index], 40);
        lv_obj_set_pos(_printButtons[index], printX[index], 180);
        styleButton(_printButtons[index], printColors[index]);
        lv_obj_add_event_cb(_printButtons[index], buttonEvent, LV_EVENT_CLICKED, reinterpret_cast<void *>(index + 3));
        lv_obj_t *label = lv_label_create(_printButtons[index]);
        lv_label_set_text(label, printLabels[index]);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(label);
    }
    setPrintButtonsVisibility(false);
    setPreheatButtonsVisibility(true);
    createChamberView();
    showMainView();
    refreshChamberDetail();
    _isDrawn = true;
    setJobActive(false);
    Serial.printf("[HEAP] createLabes free=%u min=%u maxblock=%u lvglPool=%u lvglTest=%d\n",
                  ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap(),
                  (unsigned)LV_MEM_SIZE, (int)lv_mem_test());
}

void Display::createChamberView()
{
    lv_obj_t *backButton = lv_button_create(_chamberContainer);
    lv_obj_set_size(backButton, 100, 36);
    lv_obj_set_pos(backButton, 5, 5);
    styleButton(backButton, TFT_DARKGREY);
    lv_obj_add_event_cb(backButton, backEvent, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *backLabel = lv_label_create(backButton);
    lv_label_set_text(backLabel, LV_SYMBOL_LEFT " Back");
    lv_obj_remove_flag(backLabel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(backLabel);

    lv_obj_t *title = lv_label_create(_chamberContainer);
    lv_label_set_text(title, "Enclosure");
    styleLabel(title);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 20, 12);

    _chamberTabview = lv_tabview_create(_chamberContainer);
    lv_obj_set_pos(_chamberTabview, 0, 46);
    lv_obj_set_size(_chamberTabview, SCREEN_WIDTH, SCREEN_HEIGHT - 46);
    lv_obj_set_style_bg_color(_chamberTabview, color565(BACKGROUND), 0);
    lv_obj_set_style_border_width(_chamberTabview, 0, 0);
    lv_obj_set_style_radius(_chamberTabview, 0, 0);
    lv_obj_set_style_pad_all(_chamberTabview, 0, 0);
    lv_tabview_set_tab_bar_size(_chamberTabview, 36);

    lv_obj_t *tabBar = lv_tabview_get_tab_bar(_chamberTabview);
    lv_obj_set_style_bg_color(tabBar, color565(TFT_BLACK), 0);
    lv_obj_set_style_border_width(tabBar, 0, 0);
    lv_obj_set_style_radius(tabBar, 0, 0);
    lv_obj_set_style_pad_all(tabBar, 0, 0);

    _matContainer = lv_tabview_add_tab(_chamberTabview, "Material");
    _tempContainer = lv_tabview_add_tab(_chamberTabview, "Temp");
    _manContainer = lv_tabview_add_tab(_chamberTabview, "Manual");

    for (int index = 0; index < 3; ++index)
    {
        lv_obj_t *tabButton = lv_tabview_get_tab_button(_chamberTabview, index);
        styleButton(tabButton, TFT_DARKGREY);
        lv_obj_set_style_bg_color(tabButton, color565(TFT_BLUE), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(tabButton, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_text_color(tabButton, lv_color_hex(0xFFFFFF), 0);
        lv_obj_t *tabLabel = lv_obj_get_child(tabButton, 0);
        if (tabLabel)
            lv_obj_set_style_text_color(tabLabel, lv_color_hex(0xFFFFFF), 0);
    }
    lv_obj_add_event_cb(_chamberTabview, chamberTabEvent, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_tabview_set_active(_chamberTabview, static_cast<uint32_t>(_chamberTab), LV_ANIM_OFF);

    auto stylePage = [](lv_obj_t *page) {
        lv_obj_set_style_bg_color(page, color565(BACKGROUND), 0);
        lv_obj_set_style_border_width(page, 0, 0);
        lv_obj_set_style_radius(page, 0, 0);
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_obj_set_clickable(page, false);
        lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    };
    stylePage(_matContainer);
    stylePage(_tempContainer);
    stylePage(_manContainer);

    // --- Material tab: 2x3 preset grid. ---
    const lv_coord_t matX[] = {10, 165};
    const lv_coord_t matY[] = {2, 38, 74};
    for (int index = 0; index < MATERIAL_COUNT; ++index)
    {
        _materialButtons[index] = lv_button_create(_matContainer);
        lv_obj_set_size(_materialButtons[index], 145, 32);
        lv_obj_set_pos(_materialButtons[index], matX[index % 2], matY[index / 2]);
        lv_obj_add_event_cb(_materialButtons[index], materialButtonEvent, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(index)));
        lv_obj_t *matLabel = lv_label_create(_materialButtons[index]);
        lv_label_set_text(matLabel, (String(MATERIAL_PRESETS[index].name) + " " +
                                     String(MATERIAL_PRESETS[index].setpoint) + "C")
                                        .c_str());
        lv_obj_remove_flag(matLabel, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(matLabel);
    }

    // --- Temp tab: setpoint stepper (as before) + read-only info lines. ---
    placeRowIcon(_tempContainer, &icon_chamber, 10);
    _chamberCurrentLabel = lv_label_create(_tempContainer);
    lv_label_set_text(_chamberCurrentLabel, "-- C");
    styleLabel(_chamberCurrentLabel);
    lv_obj_set_pos(_chamberCurrentLabel, 40, 2);
    _chamberSetpointLabel = lv_label_create(_tempContainer);
    lv_label_set_text(_chamberSetpointLabel, "-- C");
    lv_obj_set_style_text_color(_chamberSetpointLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(_chamberSetpointLabel, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(_chamberSetpointLabel, 40, 20);
    _tempMinusButton = createChamberStepperButton(_tempContainer, 186, 4, "-", TFT_BLUE,
                                                  stepperEvent, -CHAMBER_SETPOINT_STEP);
    _tempPlusButton = createChamberStepperButton(_tempContainer, 252, 4, "+", TFT_ORANGE,
                                                 stepperEvent, CHAMBER_SETPOINT_STEP);
    _tempInfoLabel = lv_label_create(_tempContainer);
    lv_label_set_text(_tempInfoLabel, "Print: --");
    styleLabel(_tempInfoLabel);
    lv_obj_set_pos(_tempInfoLabel, 40, 58);
    lv_obj_set_width(_tempInfoLabel, 270);
    lv_label_set_long_mode(_tempInfoLabel, LV_LABEL_LONG_DOT);
    _tempStateLabel = lv_label_create(_tempContainer);
    lv_label_set_text(_tempStateLabel, "Louvre -- Fan --");
    styleLabel(_tempStateLabel);
    lv_obj_set_pos(_tempStateLabel, 40, 76);
    lv_obj_set_width(_tempStateLabel, 270);
    lv_label_set_long_mode(_tempStateLabel, LV_LABEL_LONG_DOT);

    // --- Manual tab: louvre + fan steppers (shifted up into freed space). ---
    placeRowIcon(_manContainer, &icon_louvre, 10);
    _louvreLabel = lv_label_create(_manContainer);
    lv_label_set_text(_louvreLabel, "Louvre --");
    styleLabel(_louvreLabel);
    lv_obj_set_pos(_louvreLabel, 40, 13);
    _louvreMinusButton = createChamberStepperButton(_manContainer, 186, 4, "-", TFT_BLUE,
                                                    louvreStepperEvent, -LOUVRE_STEP);
    _louvrePlusButton = createChamberStepperButton(_manContainer, 252, 4, "+", TFT_ORANGE,
                                                   louvreStepperEvent, LOUVRE_STEP);

    placeRowIcon(_manContainer, &icon_fan, 56);
    _fanLabel = lv_label_create(_manContainer);
    lv_label_set_text(_fanLabel, "Fan --");
    styleLabel(_fanLabel);
    lv_obj_set_pos(_fanLabel, 40, 59);
    _fanMinusButton = createChamberStepperButton(_manContainer, 186, 50, "-", TFT_BLUE,
                                                 fanStepperEvent, -FAN_STEP);
    _fanPlusButton = createChamberStepperButton(_manContainer, 252, 50, "+", TFT_ORANGE,
                                                fanStepperEvent, FAN_STEP);
}

void Display::showMainView()
{
    _view = View::Main;
    if (_mainContainer)
        lv_obj_clear_flag(_mainContainer, LV_OBJ_FLAG_HIDDEN);
    if (_chamberContainer)
        lv_obj_add_flag(_chamberContainer, LV_OBJ_FLAG_HIDDEN);
}

void Display::showChamberView()
{
    if (!_mainContainer || !_chamberContainer)
        return;
    _view = View::Chamber;
    refreshChamberDetail();
    lv_obj_add_flag(_mainContainer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_chamberContainer, LV_OBJ_FLAG_HIDDEN);
    Serial.printf("[HEAP] chamber open free=%u min=%u maxblock=%u lvglTest=%d\n",
                  ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap(),
                  (int)lv_mem_test());
}

void Display::setChamberControl(ChamberControl mode)
{
    // Legacy 2-state API: Auto maps to the Temperature tab, Manual to Manual.
    setChamberTab(mode == ChamberControl::Auto ? ChamberTab::Temperature : ChamberTab::Manual);
}

void Display::setChamberTab(ChamberTab tab)
{
    const uint32_t index = static_cast<uint32_t>(tab);
    if (_chamberTabview && lv_tabview_get_tab_active(_chamberTabview) != index)
        lv_tabview_set_active(_chamberTabview, index, LV_ANIM_OFF);
    _chamberTab = tab;
    _chamberControl = (tab == ChamberTab::Manual) ? ChamberControl::Manual : ChamberControl::Auto;
    // Entering Material re-applies the selected profile setpoint.
    if (tab == ChamberTab::Material)
    {
        _chamberSetpoint = constrain(MATERIAL_PRESETS[_materialIndex].setpoint,
                                     CHAMBER_SETPOINT_MIN, CHAMBER_SETPOINT_MAX);
        _chamberSetpointSeeded = true;
    }
    refreshChamberDetail();
    pushEnclosureCommand();
}

void Display::selectMaterial(int index)
{
    if (index < 0 || index >= MATERIAL_COUNT)
        return;
    _materialIndex = index;
    _commandMaterial = (uint8_t)index;
    _chamberTab = ChamberTab::Material;
    _chamberControl = ChamberControl::Auto;
    _chamberSetpoint = constrain(MATERIAL_PRESETS[index].setpoint,
                                 CHAMBER_SETPOINT_MIN, CHAMBER_SETPOINT_MAX);
    _chamberSetpointSeeded = true;
    _lastAutoFilament = normalizeFilament(_octoFilament);
    refreshChamberDetail();
    pushEnclosureCommand();
}

void Display::autoSelectMaterial(const String &filament, bool jobActive)
{
    const String norm = normalizeFilament(filament);
    const bool wasActive = _wasJobActive;
    _wasJobActive = jobActive;
    if (filament.length())
        _octoFilament = filament;
    if (!jobActive)
        return;
    if (_chamberTab != ChamberTab::Material)
    {
        const uint8_t detectedMaterial = classifyFilament(filament);
        if (_commandMaterial != detectedMaterial)
        {
            _commandMaterial = detectedMaterial;
            _enclosureCommandDirty = true;
        }
        _lastAutoFilament = norm;
        return;
    }

    const bool startEdge = !wasActive;
    const bool changed = norm.length() > 0 && norm != _lastAutoFilament;
    if (!startEdge && !changed)
        return;
    _lastAutoFilament = norm;

    const int matched = matchMaterialIndex(filament);
    if (matched >= 0)
    {
        _materialIndex = matched;
        _commandMaterial = (uint8_t)matched;
        _chamberSetpoint = constrain(MATERIAL_PRESETS[matched].setpoint,
                                     CHAMBER_SETPOINT_MIN, CHAMBER_SETPOINT_MAX);
    }
    else
    {
        _commandMaterial = classifyFilament(filament);
        _chamberSetpoint = MATERIAL_ASA_SETPOINT;
    }

    _chamberSetpointSeeded = true;
    _enclosureCommandDirty = true;
    refreshChamberDetail();
}

void Display::fillEnclosureCommand(EnclosureCommand &cmd) const
{
    cmd.mode = (_chamberTab == ChamberTab::Manual) ? ENC_MODE_MANUAL : ENC_MODE_AUTO;
    cmd.setpoint = (uint8_t)constrain(_chamberSetpoint, 0, 255);
    cmd.louvrePct = (uint8_t)constrain(_louvreOpen, 0, 100);
    cmd.fanPct = (uint8_t)constrain(_fanSpeed, 0, 100);
    cmd.material = _commandMaterial;
    cmd.jobActive = _jobActive ? 1 : 0;
}

void Display::pushEnclosureCommand()
{
    EnclosureCommand cmd = {};
    fillEnclosureCommand(cmd);
    enclosureLinkSend(cmd);
}

void Display::enclosureUpdate(const EnclosureSnapshot &snap)
{
    if (snap.valid)
        _enclosure = snap;
    refreshChamberDetail();
    refreshEnclosureLinkHeader();
}

void Display::refreshEnclosureLinkHeader()
{
    if (!_encLinkIcon)
        return;
    const bool up = _enclosure.valid && (millis() - _enclosure.rxMs < ENC_LINK_STALE_MS);
    if (up == _encLinkUp)
        return;
    _encLinkUp = up;
    lv_obj_set_style_image_recolor(_encLinkIcon, color565(up ? TFT_GREEN : 0x808080), 0);
}

void Display::chamberSetpointDelta(int delta)
{
    // Desired temperature is the automatic-control input; ignore in Manual.
    if (_chamberTab == ChamberTab::Manual)
        return;
    _chamberSetpoint = constrain(_chamberSetpoint + delta, CHAMBER_SETPOINT_MIN, CHAMBER_SETPOINT_MAX);
    _chamberSetpointSeeded = true;
    refreshChamberDetail();
    pushEnclosureCommand();
}

void Display::louvreDelta(int delta)
{
    // Louvre is the manual-control input; ignore outside Manual tab.
    if (_chamberTab != ChamberTab::Manual)
        return;
    _louvreOpen = constrain(_louvreOpen + delta, LOUVRE_MIN, LOUVRE_MAX);
    if (_louvreOpen < FAN_LOUVRE_MIN_OPEN)
        _fanSpeed = FAN_MIN;
    refreshChamberDetail();
    pushEnclosureCommand();
}

void Display::fanDelta(int delta)
{
    // Fan is the manual-control input; ignore outside Manual tab.
    if (_chamberTab != ChamberTab::Manual)
        return;
    // Fan stays at 0 and ignores input until the louvre is open enough.
    if (_louvreOpen < FAN_LOUVRE_MIN_OPEN)
    {
        _fanSpeed = FAN_MIN;
        refreshChamberDetail();
        pushEnclosureCommand();
        return;
    }
    _fanSpeed = constrain(_fanSpeed + delta, FAN_MIN, FAN_MAX);
    refreshChamberDetail();
    pushEnclosureCommand();
}

void Display::refreshChamberDetail()
{
    if (!_chamberSetpointLabel || !_chamberCurrentLabel || !_louvreLabel || !_fanLabel)
        return;
    // The chamber pages are hidden while the main view is shown; refreshing
    // hidden labels is pure heap churn (see setLabelTextOnce). Telemetry and
    // auto-select state are still latched by the callers.
    if (!_isDrawn || _view != View::Chamber)
        return;
    const bool isManual = _chamberTab == ChamberTab::Manual;
    const lv_color_t dim = lv_color_hex(0x808080);
    const lv_color_t bright = lv_color_hex(0xFFFFFF);

    // Material tab: highlight selected preset.
    for (int index = 0; index < MATERIAL_COUNT; ++index)
    {
        if (!_materialButtons[index])
            continue;
        styleButton(_materialButtons[index], index == _materialIndex ? TFT_DARKGREEN : TFT_DARKGREY);
    }

    const bool fresh = _enclosure.valid && (millis() - _enclosure.rxMs < ENC_LINK_STALE_MS);
    const String c3state = fresh ? ("Louvre " + String(_enclosure.louvrePct) + "% Fan " +
                                    String(_enclosure.fanPct) + "%")
                                 : String("Louvre -- Fan --");
    const String printMat = _octoFilament.length() ? _octoFilament : String("--");
    if (_tempInfoLabel)
        setLabelTextOnce(_tempInfoLabel, "Print: " + printMat);
    if (_tempStateLabel)
    {
        setLabelTextOnce(_tempStateLabel, "C3: " + c3state);
        lv_obj_set_style_text_color(_tempStateLabel, fresh ? bright : dim, 0);
    }

    // Temperature tab: actual on top, desired setpoint below; stepper active.
    setLabelTextOnce(_chamberSetpointLabel, "Des " + String(_chamberSetpoint) + " C");
    lv_obj_clear_flag(_chamberSetpointLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(_chamberSetpointLabel, isManual ? dim : bright, 0);
    if (_tempMinusButton)
        styleButton(_tempMinusButton, isManual ? TFT_DARKGREY : TFT_BLUE);
    if (_tempPlusButton)
        styleButton(_tempPlusButton, isManual ? TFT_DARKGREY : TFT_ORANGE);

    // Manual tab: adjustable louvre/fan.
    setLabelTextOnce(_louvreLabel, "Louvre " + String(_louvreOpen) + " %");
    const bool fanLocked = _louvreOpen < FAN_LOUVRE_MIN_OPEN;
    setLabelTextOnce(_fanLabel, String("Fan ") + String(_fanSpeed) + " %" + (fanLocked ? " (locked)" : ""));
    lv_obj_set_style_text_color(_louvreLabel, isManual ? bright : dim, 0);
    const bool fanDimmed = !isManual || fanLocked;
    lv_obj_set_style_text_color(_fanLabel, fanDimmed ? dim : bright, 0);
    if (_louvreMinusButton)
        styleButton(_louvreMinusButton, isManual ? TFT_BLUE : TFT_DARKGREY);
    if (_louvrePlusButton)
        styleButton(_louvrePlusButton, isManual ? TFT_ORANGE : TFT_DARKGREY);
    if (_fanMinusButton)
        styleButton(_fanMinusButton, fanDimmed ? TFT_DARKGREY : TFT_BLUE);
    if (_fanPlusButton)
        styleButton(_fanPlusButton, fanDimmed ? TFT_DARKGREY : TFT_ORANGE);
}

void Display::setButtonActions(Action preheat215, Action preheat230, Action preheatOff,
                               Action pause, Action stop, Action resume)
{
    buttonActions[0] = preheat215;
    buttonActions[1] = preheat230;
    buttonActions[2] = preheatOff;
    buttonActions[3] = pause;
    buttonActions[4] = stop;
    buttonActions[5] = resume;
}

void Display::setButtonVisibility(lv_obj_t **buttons, size_t count, bool visible)
{
    for (size_t index = 0; index < count; ++index)
    {
        if (visible)
            lv_obj_clear_flag(buttons[index], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(buttons[index], LV_OBJ_FLAG_HIDDEN);
    }
}

void Display::clearJobDisplay()
{
    _jobFileName = "";
    _displayedFileName = "";
    _lastRemaining = "";
    _progressCompletion = -1;
    _lastLayerCurrent = -1;
    _lastLayerTotal = -1;
    _lastLastDuration = "";
    setLabelTextOnce(_progressLabel, "-- %");
    setLabelTextOnce(_layerLabel, "L--/--");
    setLabelTextOnce(_timeLabel, "--");
    setLabelTextOnce(_lastLayerLabel, "L --");
    setLabelTextOnce(_filamentLabel, "--");
    setLabelTextOnce(_nozzleLabel, "--");
    if (_progressBar)
    {
        lv_bar_set_value(_progressBar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(_progressBar, color565(TFT_GREEN), LV_PART_INDICATOR);
    }
}

void Display::applyMainJobLayout(bool active)
{
    if (active)
    {
        lv_obj_set_pos(_toolPanel, 5, 35);
        lv_obj_set_size(_toolPanel, 130, 36);
        lv_obj_set_pos(_bedPanel, 5, 76);
        lv_obj_set_size(_bedPanel, 130, 36);
        lv_obj_set_pos(_chamberPanel, 5, 117);
        lv_obj_set_size(_chamberPanel, 130, 36);
        lv_obj_set_pos(_progressPanel, 140, 35);
        lv_obj_set_size(_progressPanel, 175, 48);
        lv_obj_set_pos(_filamentPanel, 140, 88);
        lv_obj_set_size(_filamentPanel, 175, 30);
        lv_obj_set_pos(_filePanel, 140, 123);
        lv_obj_set_size(_filePanel, 175, 30);
        lv_obj_set_hidden(_progressPanel, false);
        lv_obj_set_hidden(_filamentPanel, false);
        lv_obj_set_hidden(_filePanel, false);
        lv_obj_set_hidden(_progressBar, false);
        lv_obj_set_pos(_progressBar, 2, 160);
        lv_obj_set_width(_fileNameLabel, 159);
        lv_label_set_long_mode(_fileNameLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(_fileNameLabel, LV_ALIGN_LEFT_MID, 4, 0);
        for (int index = 0; index < 3; ++index)
        {
            lv_obj_set_pos(_preheatButtons[index], 10 + index * 90, 180);
            lv_obj_set_pos(_printButtons[index], index == 0 ? 5 : (index == 1 ? 100 : 205), 180);
        }
    }
    else
    {
        lv_obj_set_pos(_toolPanel, 5, 35);
        lv_obj_set_size(_toolPanel, 145, 50);
        lv_obj_set_pos(_bedPanel, 170, 35);
        lv_obj_set_size(_bedPanel, 145, 50);
        lv_obj_set_pos(_chamberPanel, 5, 95);
        lv_obj_set_size(_chamberPanel, 145, 50);
        lv_obj_set_pos(_filePanel, 170, 95);
        lv_obj_set_size(_filePanel, 145, 50);
        lv_obj_set_hidden(_progressPanel, true);
        lv_obj_set_hidden(_filamentPanel, true);
        lv_obj_set_hidden(_progressBar, true);
        lv_obj_set_width(_fileNameLabel, 135);
        lv_label_set_long_mode(_fileNameLabel, LV_LABEL_LONG_DOT);
        setLabelTextOnce(_fileNameLabel, "No active print");
        lv_obj_center(_fileNameLabel);
        for (int index = 0; index < 3; ++index)
        {
            lv_obj_set_pos(_preheatButtons[index], 10 + index * 90, 190);
            lv_obj_set_pos(_printButtons[index], index == 0 ? 5 : (index == 1 ? 100 : 205), 190);
        }
    }
}

void Display::setJobActive(bool active)
{
    const bool changed = !_jobLayoutInitialized || _jobLayoutActive != active;
    _jobActive = active;
    if (changed)
    {
        _jobLayoutInitialized = true;
        _jobLayoutActive = active;
        if (!active)
            clearJobDisplay();
        applyMainJobLayout(active);
        setPreheatButtonsVisibility(!active);
        setPrintButtonsVisibility(active);
    }
    if (changed || _enclosureCommandDirty)
    {
        _enclosureCommandDirty = false;
        pushEnclosureCommand();
    }
}

void Display::setPreheatButtonsVisibility(bool visible)
{
    setButtonVisibility(_preheatButtons, 3, visible);
}

void Display::setPrintButtonsVisibility(bool visible)
{
    setButtonVisibility(_printButtons, 3, visible);
}

void Display::drawWiFiSignal(int32_t rssi)
{
    if (!_wifiBars[0])
        return;
    int bars = rssi > -55 ? 4 : rssi > -70 ? 3
                            : rssi > -85   ? 2
                            : rssi > -100  ? 1
                                           : 0;
    const uint16_t levelColor = bars >= 4 ? TFT_GREEN : bars == 3 ? TFT_YELLOW
                                              : bars == 2        ? TFT_ORANGE
                                                                 : TFT_RED;
    for (size_t index = 0; index < 4; ++index)
    {
        if (!_wifiBars[index])
            continue;
        lv_obj_set_style_bg_color(_wifiBars[index], color565(levelColor), 0);
        if (static_cast<int>(index) < bars)
            lv_obj_clear_flag(_wifiBars[index], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(_wifiBars[index], LV_OBJ_FLAG_HIDDEN);
    }
}

void Display::printerStatisticUpdate(const PrinterSnapshot &snap)
{
    if (!_stateLabel || !_toolLabel || !_bedLabel || !_chamberLabel)
        return;
    if (!snap.state.length())
        return;
    setLabelTextOnce(_stateLabel, "State: " + snap.state);
    updateTempLabel(_toolLabel, snap.toolActual, snap.toolTarget);    updateTempLabel(_bedLabel, snap.bedActual, snap.bedTarget);
    if (!_chamberSetpointSeeded && snap.chamberTarget >= CHAMBER_SETPOINT_MIN &&
        snap.chamberTarget <= CHAMBER_SETPOINT_MAX && snap.chamberTarget != 0)
    {
        _chamberSetpoint = snap.chamberTarget;
        _chamberSetpointSeeded = true;
    }
    // Main panel mirrors nozzle/bed ("actual / desired") in Auto, actual only in Manual.
    if (_chamberControl == ChamberControl::Auto)
        updateTempLabel(_chamberLabel, snap.chamberActual, _chamberSetpoint);
    else
        updateTempLabel(_chamberLabel, snap.chamberActual, 0);
    if (_chamberCurrentLabel)
        setLabelTextOnce(_chamberCurrentLabel, String(snap.chamberActual, 1) + " C");
    refreshChamberDetail();
}

void Display::jobUpdate(const PrinterSnapshot &snap)
{
    // Material-auto runs even before labels exist so profile state is correct.
    autoSelectMaterial(snap.filament, snap.printing || snap.paused);
    if (!_progressBar || !_progressLabel || !_layerLabel || !_timeLabel || !_lastLayerLabel || !_filamentLabel || !_nozzleLabel || !_fileNameLabel)
        return;
    const String fileName = snap.fileName;
    const int slash = fileName.lastIndexOf('/');
    const int backslash = fileName.lastIndexOf('\\');
    const int cut = slash > backslash ? slash : backslash;
    const String baseName = cut >= 0 ? fileName.substring(cut + 1) : fileName;
    if (_displayedFileName != baseName)
    {
        _displayedFileName = baseName;
        lv_label_set_text(_fileNameLabel, baseName.length() ? baseName.c_str() : "--");
    }
    setLabelTextOnce(_filamentLabel, snap.filament);
    setLabelTextOnce(_nozzleLabel, snap.nozzle);
    if (!snap.printing && !snap.paused)
        return;
    float progress = snap.progress;
    if (!isfinite(progress))
        progress = 0;
    progress = constrain(progress, 0, 100);
    String remaining = snap.remaining;
    const int layerCurrent = snap.layerCurrent;
    const int layerTotal = snap.layerTotal;
    const bool showLayer = layerTotal > 0 && layerCurrent > 0;
    String lastDuration = trimDuration(snap.lastLayerDuration);
    if (lastDuration.length() == 0 && snap.lastLayerDurationSeconds > 0)
        lastDuration = formatHMS(snap.lastLayerDurationSeconds);
    String progressText;
    if (showLayer)
        progressText = String(static_cast<int>(progress)) + "%";
    else
        progressText = String(progress, 1) + " %";
    String layerText;
    if (showLayer)
        layerText = "L" + String(layerCurrent) + "/" + String(layerTotal);
    else
        layerText = "L--/--";
    const String lastLayerText = String("L ") + (lastDuration.length() ? lastDuration : "--");
    if (_jobFileName != fileName || _progressCompletion != progress ||
        (showLayer && (_lastLayerCurrent != layerCurrent || _lastLayerTotal != layerTotal)) ||
        (!showLayer && (_lastLayerCurrent > 0 || _lastLayerTotal > 0)) ||
        _lastLastDuration != lastDuration)
    {
        const uint32_t color = progress < 25 ? TFT_RED : progress < 50 ? TFT_ORANGE
                                              : progress < 75   ? TFT_YELLOW
                                                               : TFT_GREEN;
        lv_bar_set_value(_progressBar, static_cast<int32_t>(progress), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(_progressBar, color565(color), LV_PART_INDICATOR);
        lv_label_set_text(_progressLabel, progressText.c_str());
        lv_label_set_text(_layerLabel, layerText.c_str());
        lv_label_set_text(_lastLayerLabel, lastLayerText.c_str());
        _jobFileName = fileName;
        _progressCompletion = progress;
        _lastLayerCurrent = showLayer ? layerCurrent : -1;
        _lastLayerTotal = showLayer ? layerTotal : -1;
        _lastLastDuration = lastDuration;
    }
    if (_lastRemaining != remaining || _jobFileName != fileName)
    {
        lv_label_set_text(_timeLabel, remaining.c_str());
        _lastRemaining = remaining;
    }
}
