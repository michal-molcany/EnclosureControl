#include "display.h"

#include <WiFi.h>
#include <XPT2046_Touchscreen.h>

#include "enclosure_version.h" // ENC_FW_VERSION_STR / ENC_PROTO_VERSION

#include "enclosure_link.h"

#include "icons/icon_api.h"
#include "icons/icon_bed.h"
#include "icons/icon_bed_waves.h"
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

// Same order as MATERIAL_PRESETS, so index N is the same material in all three.
const int Display::TOOL_PRESETS[MATERIAL_COUNT] = {
    TOOL_PLA_TEMP,
    TOOL_PETG_TEMP,
    TOOL_ASA_TEMP,
    TOOL_ABS_TEMP,
    TOOL_TPU_TEMP,
    TOOL_NYLON_TEMP,
};

const int Display::BED_PRESETS[MATERIAL_COUNT] = {
    BED_PLA_TEMP,
    BED_PETG_TEMP,
    BED_ASA_TEMP,
    BED_ABS_TEMP,
    BED_TPU_TEMP,
    BED_NYLON_TEMP,
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
    // LVGL partial-render buffer, in scanlines. Static DRAM here is extremely
    // tight (see the sizing note below), so this was reduced from 10 to 6
    // lines to make room for the HTTP API fallback's link-time footprint.
    //
    // Sizing: on the esp32dev target the linker had only 624 bytes of spare
    // internal DRAM before the enclosure-API client was added, which needs
    // ~2.2 KB of statics (HTTPClient + ArduinoJson + WiFi). This buffer is
    // 320 * LINES * sizeof(lv_color_t); at 6 lines it frees ~3.8 KB, which
    // restores real headroom.
    //
    // The trade is flush count, not correctness: LVGL redraws in buffer-sized
    // chunks, so fewer lines means more, smaller SPI transactions per
    // full-screen update. The panel has its own GRAM, so nothing is lost in
    // quality - only a little throughput on redraw-heavy views.
    static constexpr int kDrawLines = 6;
    lv_color_t drawBuffer[320 * kDrawLines];
    // Job buttons on the main view (pause/stop/resume), in JobCommand order.
    Display::Action buttonActions[3] = {};
    // Main-view tool preheats: PLA / PETG / ASA / off.
    Display::Action toolActions[4] = {};
    // Four material buttons across the 320px width; applyMainJobLayout() reuses these,
    // so they cannot live inside createLabes(). Geometry is
    // 5 + 73 + 6 + 73 + 6 + 73 + 6 + 73 + 5 = 320: equal margins and gaps, so the
    // row is symmetric instead of running flush to the right edge.
    const lv_coord_t preheatX[4] = {5, 84, 163, 242};
    constexpr lv_coord_t kPreheatButtonWidth = 73;
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
        if (lv_event_get_code(event) != LV_EVENT_CLICKED)
            return;
        const uintptr_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
        if (index >= 3)
            return;
        // Pause (0) and Cancel (1) need a Yes/Cancel confirm; Resume (2) fires
        // immediately. buttonActions stays the single place that knows how to
        // send the command - the dialog only delays index 0/1 until Yes.
        if ((index == 0 || index == 1) && activeDisplay)
        {
            activeDisplay->showJobConfirm(index == 0 ? PAUSE_JOB : STOP_JOB);
            return;
        }
        if (buttonActions[index])
            buttonActions[index]();
    }

    void confirmYesEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) == LV_EVENT_CLICKED && activeDisplay)
            activeDisplay->confirmPendingJob();
    }

    void confirmCancelEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) == LV_EVENT_CLICKED && activeDisplay)
            activeDisplay->hideJobConfirm();
    }

    void confirmOverlayEvent(lv_event_t *event)
    {
        // Tap outside the box counts as Cancel. Box children have the box (or
        // deeper) as target, so only a direct tap on the dim background lands
        // here with the overlay itself as target.
        if (lv_event_get_code(event) != LV_EVENT_CLICKED || !activeDisplay)
            return;
        if (lv_event_get_target(event) == lv_event_get_current_target(event))
            activeDisplay->hideJobConfirm();
    }

    void calibrationButtonEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) == LV_EVENT_CLICKED && activeDisplay)
            activeDisplay->calibrationButtonTapped();
    }

    void toolButtonEvent(lv_event_t *event)
    {
        // Main-view quick preheats: fixed 215 / 230 / off. The nozzle screen
        // reaches the same API with presets and a stepper; these stay because they
        // are one tap instead of two.
        if (lv_event_get_code(event) == LV_EVENT_CLICKED)
        {
            const uintptr_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
            if (index < 4 && toolActions[index])
                toolActions[index]();
        }
    }

    void bedPanelEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) == LV_EVENT_CLICKED && activeDisplay)
            activeDisplay->showBedView();
    }

    void toolPanelEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) == LV_EVENT_CLICKED && activeDisplay)
            activeDisplay->showToolView();
    }

    // The preheat screens are generated from one template, so a single event
    // callback serves both. Each button carries a pointer to one of these to say
    // which target it belongs to and what it does. A static array keeps those
    // pointers valid for the life of the program, which is what lets them travel
    // through LVGL as user_data.
    struct PreheatBinding
    {
        Display::PreheatKind kind;
        int value;
    };
    PreheatBinding preheatBindings[2 * (MATERIAL_COUNT + 3)];
    uint8_t preheatBindingCount = 0;

    PreheatBinding *addPreheatBinding(Display::PreheatKind kind, int value)
    {
        if (preheatBindingCount >= sizeof(preheatBindings) / sizeof(preheatBindings[0]))
            return nullptr;
        PreheatBinding *binding = &preheatBindings[preheatBindingCount++];
        binding->kind = kind;
        binding->value = value;
        return binding;
    }

    const PreheatBinding *eventBinding(lv_event_t *event)
    {
        return static_cast<const PreheatBinding *>(lv_event_get_user_data(event));
    }

    void preheatPresetEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) != LV_EVENT_CLICKED || !activeDisplay)
            return;
        const PreheatBinding *binding = eventBinding(event);
        if (!binding)
            return;
        Serial.printf("[PREHEAT] preset kind=%d idx=%d\n", (int)binding->kind, binding->value);
        activeDisplay->selectPreheatPreset(binding->kind, binding->value);
    }

    void preheatStepperEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) != LV_EVENT_CLICKED || !activeDisplay)
            return;
        const PreheatBinding *binding = eventBinding(event);
        if (!binding)
            return;
        activeDisplay->stepPreheatTarget(binding->kind, binding->value);
    }

    void preheatOffEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) == LV_EVENT_CLICKED && activeDisplay)
        {
            const PreheatBinding *binding = eventBinding(event);
            if (!binding)
                return;
            Serial.printf("[PREHEAT] off kind=%d\n", (int)binding->kind);
            activeDisplay->setPreheatTarget(binding->kind, 0);
        }
    }

    void chamberPanelEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) == LV_EVENT_CLICKED && activeDisplay)
            activeDisplay->showChamberView();
    }

    void filesPanelEvent(lv_event_t *event)
    {
        // The file panel doubles as the files entry while idle; activeDisplay
        // re-checks _jobActive, so taps on a running job's filename are inert.
        if (lv_event_get_code(event) == LV_EVENT_CLICKED && activeDisplay)
            activeDisplay->openFilesView();
    }

    void fileRowEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) != LV_EVENT_CLICKED || !activeDisplay)
            return;
        const intptr_t index = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
        if (index >= 0 && index < MAX_PRINT_FILES)
            activeDisplay->selectFile(static_cast<int>(index));
    }

    void filesPrintEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) == LV_EVENT_CLICKED && activeDisplay)
            activeDisplay->showPrintConfirm();
    }

    void filesInfoEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) == LV_EVENT_CLICKED && activeDisplay)
            activeDisplay->showFileInfo();
    }

    // A touch anywhere on the details panel returns to the list. There is nothing
// interactive in the panel - it is a table of values - so the whole screen is
// the back target.
//
// Gated on _view inside the handler rather than by attaching and detaching the
// callback, because LVGL bubbles events from the object that was touched up
// through its parents: an unconditional handler here would also fire for a tap
// on a file row and bounce the user out of the list they were choosing from.
void filesPanelTouchEvent(lv_event_t *event)
    {
        (void)event;
        if (activeDisplay)
            activeDisplay->filesPanelTouched();
    }

    // Tap box for the wifi-signal / info-screen gesture. x=270 is just left of the
// first bar (275); y=26 clears the header separator at y=28.
static constexpr lv_coord_t WIFI_TAP_X = 270;
static constexpr lv_coord_t WIFI_TAP_Y = 26;

// The info screen is opened by tapping the wifi signal in the header.
//
// Handled on the main CONTAINER with a coordinate test, rather than on a
// transparent hit-area object sitting over the bars: it saves an LVGL object,
// which is worth having on a board whose pool is capped at 60 KB. The cost is
// that the handler sees every press on the main view, because LVGL bubbles
// events from the touched child (the chamber/bed/tool/file panels) up through
// its parents - so it must check WHERE the touch landed.
//
// PRESSED, not CLICKED: this is an entry gesture on a fixed target, and reading
// the touch point is only reliable on the press.
    void mainContainerTapEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) != LV_EVENT_PRESSED || !activeDisplay)
            return;
        // Only from the main view: on any other view the header is not shown, and
        // a stray press should never summon a full-screen diagnostic.
        if (activeDisplay->currentView() != Display::View::Main)
            return;
        lv_point_t point;
        lv_indev_t *indev = lv_event_get_indev(event);
        if (!indev)
            return;
        // Returns void in this LVGL, so there is no failure case to test.
        lv_indev_get_point(indev, &point);
        // The signal group: four 7 px bars at x=275..315, bottom-aligned to y=22.
        // The box is deliberately looser than the bars themselves - 7 px is a
        // fiddly target on a 320x240 panel - and stops short of the link icons at
        // x=230 and x=252, which are left free for separate taps later.
        if (point.x >= WIFI_TAP_X && point.x <= SCREEN_WIDTH && point.y < WIFI_TAP_Y)
            activeDisplay->showInfoView();
    }

    // Back out of the info screen.
    void infoBackEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) == LV_EVENT_CLICKED && activeDisplay)
            activeDisplay->showMainView();
    }

    // Tap anywhere on the info screen also closes it.
    //
    // LV_EVENT_CLICKED, not the PRESSED that filesPanelTouchEvent uses, and the
    // difference matters: this screen is a grid of labels with no draggable or
    // interactive content, but a PRESSED handler also fires on the way into a
    // scroll or a stray drag, so a finger resting on the glass would dismiss it
    // before it had finished registering. LVGL suppresses CLICKED when a press
    // turns into a scroll, which is exactly the behaviour wanted here.
    void infoTapEvent(lv_event_t *event)
    {
        if (lv_event_get_code(event) == LV_EVENT_CLICKED && activeDisplay)
            activeDisplay->closeInfoView();
    }

    // ---- formatting for the file-info view -------------------------------
    //
    // A 0 coming from OctoPrint means it does not know (no analysis in the file,
    // or never printed) rather than a real zero. Rendering that as "print time
    // 0s" or "filament 0 mm" would be stating a fact we do not have, so each
    // helper returns a fallback instead.

    String formatDuration(double seconds)
    {
        if (seconds < 1)
            return "unknown";
        const long total = (long)(seconds + 0.5);
        const long h = total / 3600;
        const long m = (total % 3600) / 60;
        const long s = total % 60;
        char buf[32];
        if (h > 0)
            snprintf(buf, sizeof(buf), "%ldh %02ldm", h, m);
        else if (m > 0)
            snprintf(buf, sizeof(buf), "%ldm %02lds", m, s);
        else
            snprintf(buf, sizeof(buf), "%lds", s);
        return String(buf);
    }

    String formatBytes(uint32_t bytes)
    {
        if (bytes == 0)
            return "unknown";
        char buf[24];
        if (bytes >= 1048576UL)
            snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
        else if (bytes >= 1024)
            snprintf(buf, sizeof(buf), "%.0f kB", bytes / 1024.0);
        else
            snprintf(buf, sizeof(buf), "%lu B", (unsigned long)bytes);
        return String(buf);
    }

    // The two lines a file row shows: the name, then the details most useful when
// choosing what to print - material, how long it takes, how big it is.
//
// Hyphens rather than a middle dot as the separator: the enabled font subset
// carries ASCII + degree + bullet only, and a missing glyph would render as a
// blank box on the one row with the least room to explain itself. Unknown values
// are left out rather than printed as zeros.
String fileRowText(const PrintFile &entry)
{
    String detail = entry.material[0] ? String(entry.material) : String("material ?");
    if (entry.estimatedPrintTime > 1)
        detail += " - " + formatDuration(entry.estimatedPrintTime);
    if (entry.size > 0)
        detail += " - " + formatBytes(entry.size);
    return entry.name + String("\n") + detail;
}

// Days since the epoch for a civil date (Howard Hinnant), the inverse of the
// civil-from-days used in formatUploadDate() below.
static int64_t daysFromCivil(int64_t y, unsigned m, unsigned d)
{
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + ((m > 2) ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

// Day-of-month of the last Sunday of a 31-day month. March and October are both
// 31 days, which is why this takes no last-day argument - and why it is only
// correct for the two months the EU rule actually uses.
static unsigned lastSundayOf31DayMonth(int64_t year, unsigned month)
{
    const unsigned lastDay = 31;
    const int64_t days = daysFromCivil(year, month, lastDay);
    // Epoch day 0 was a Thursday, so index 3 is Sunday.
    const unsigned weekday = (unsigned)((days % 7 + 7) % 7);
    return lastDay - ((weekday + 7 - 3) % 7);
}

// Was this timestamp inside European summer time? The EU rule, applied to the
// timestamp being formatted rather than to a system clock: this board has no
// clock, and one stuck in 1970 would call every summer timestamp winter time.
static bool isEuSummerTime(uint32_t unixSeconds)
{
    const int64_t days = (int64_t)(unixSeconds / 86400UL);
    int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned month = mp < 10 ? mp + 3 : mp - 9;
    const int64_t year = y + (month <= 2 ? 1 : 0);

    // 01:00 UTC on the last Sunday of March, to 01:00 UTC on the last Sunday of
    // October.
    const uint32_t startUtc =
        (uint32_t)(daysFromCivil(year, 3, lastSundayOf31DayMonth(year, 3)) * 86400 + 3600);
    const uint32_t endUtc =
        (uint32_t)(daysFromCivil(year, 10, lastSundayOf31DayMonth(year, 10)) * 86400 + 3600);
    return unixSeconds >= startUtc && unixSeconds < endUtc;
}

// Date/time in Europe/Bratislava local time, Slovak format (D.M.YYYY, 24-hour).
//
// CET (UTC+1) in winter, CEST (UTC+2) in summer, with the EU daylight saving
// rule applied to the value itself - see const.h for why this is hand-rolled
// rather than done with localtime().
//
// The zone is printed next to the value on purpose: an unqualified time here
// would be an hour wrong for half the year, and a wrong timestamp that looks
// right is worse than one that admits what it is.
//
// No diacritics: the font subset is ASCII + degree + bullet, so "máj" or
// "štvrtok" would render as missing glyphs. Hence numeric rather than spelled
// out with Slovak month names.
String formatUploadDate(uint32_t unixSeconds)
{
    if (unixSeconds == 0)
        return "unknown";

    const bool summer = isEuSummerTime(unixSeconds);
    const int64_t stamp = (int64_t)unixSeconds + (summer ? 7200 : 3600);
    const uint32_t secs = (uint32_t)(stamp % 86400);
    const uint32_t days = (uint32_t)(stamp / 86400);

    int64_t z = (int64_t)days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    const int64_t year = y + (m <= 2 ? 1 : 0);

    char buf[48];
    snprintf(buf, sizeof(buf), "%02u.%02u.%04d  %02u:%02u %s", d, m, (int)year,
             secs / 3600, (secs % 3600) / 60, summer ? "CEST" : "CET");
    return String(buf);
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

    // The chamber steppers pass their delta as user_data; the preheat screens pass a
    // pointer to a binding instead (see PreheatBinding), so both fit in one void*.
    void *stepData(int delta)
    {
        return reinterpret_cast<void *>(static_cast<intptr_t>(delta));
    }

lv_obj_t *createChamberStepperButton(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                        const char *text, uint16_t color,
                                        lv_event_cb_t handler, void *userData)
    {
        lv_obj_t *button = lv_button_create(parent);
        lv_obj_set_size(button, 60, 36);
        lv_obj_set_pos(button, x, y);
        styleButton(button, color);
        lv_obj_add_event_cb(button, handler, LV_EVENT_CLICKED, userData);
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

    // Pending-overlay code for "cancel PID calibration". Deliberately outside
    // the JobCommand range (1-6, see const.h) so one _pendingJob slot can serve
    // both dialogs without retargeting the wrong action.
    constexpr int CAL_CANCEL = 7;

    bool calPhaseActive(uint8_t phase)
    {
        return phase == ENC_CAL_PHASE_PREHEATING || phase == ENC_CAL_PHASE_SETTLING ||
               phase == ENC_CAL_PHASE_RELAY;
    }

    const char *calPhaseText(uint8_t phase)
    {
        switch (phase)
        {
        case ENC_CAL_PHASE_PREHEATING:
            return "Preheating bed";
        case ENC_CAL_PHASE_SETTLING:
            return "Settling";
        case ENC_CAL_PHASE_RELAY:
            return "Relay tuning";
        case ENC_CAL_PHASE_DONE:
            return "Complete";
        case ENC_CAL_PHASE_ABORTED:
            return "Aborted";
        case ENC_CAL_PHASE_NONE:
        default:
            return "Idle";
        }
    }

    const char *calErrorText(uint8_t code)
    {
        switch (code)
        {
        case ENC_CAL_ERR_CANCELLED:
            return "Cancelled";
        case ENC_CAL_ERR_EMERGENCY:
            return "Emergency stop at 55 C";
        case ENC_CAL_ERR_CHAMBER_HIGH:
            return "Chamber exceeded 50 C";
        case ENC_CAL_ERR_BED_TIMEOUT:
            return "Bed did not reach 90 C";
        case ENC_CAL_ERR_OCTO_LOST:
            return "OctoPrint link lost";
        case ENC_CAL_ERR_DATA_STALE:
            return "OctoPrint data went stale";
        case ENC_CAL_ERR_PRINT_STARTED:
            return "A print started";
        case ENC_CAL_ERR_TIMEOUT:
            return "Timed out";
        case ENC_CAL_ERR_BAD_READING:
            return "Temperature reading unusable";
        case ENC_CAL_ERR_OTHER:
            return "Stopped by the C3";
        case ENC_CAL_ERR_NONE:
        default:
            return "Unknown";
        }
    }

    void formatCx10(int16_t value, char *buf, size_t n)
    {
        if (value == ENC_CHAMBER_INVALID)
        {
            snprintf(buf, n, "--");
            return;
        }
        const int whole = value / 10;
        const int tenth = abs(value % 10);
        snprintf(buf, n, "%d.%d C", whole, tenth);
    }

    void formatCalElapsed(uint16_t seconds, char *buf, size_t n)
    {
        const unsigned minutes = seconds / 60U;
        const unsigned secs = seconds % 60U;
        if (minutes >= 60U)
            snprintf(buf, n, "%u:%02u:%02u", minutes / 60U, minutes % 60U, secs);
        else
            snprintf(buf, n, "%u:%02u", minutes, secs);
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
    // The view containers are full-screen siblings drawn under the splash, so
    // repainting the screen background alone is not enough to make this opaque:
    // hide them while the splash is up. No-ops during boot, when they are still
    // null.
    hideAllViews();
    // A stale Yes must never fire after an OTA splash: dismiss the job confirm
    // while the splash is up. hideJobConfirm() is null-safe before createLabes().
    hideJobConfirm();

    _splashFrame = (_splashFrame + 1) % 12;
    lv_arc_set_rotation(_splashSpinner, 270 + _splashFrame * 30);
    lv_label_set_text(_splashLabel, message.c_str());
    lv_refr_now(_display);
}

void Display::hideSplash()
{
    // Mirrors the teardown in createLabes(), but keeps the view containers that
    // were built after the splash.
    if (_splashSpinner)
    {
        lv_obj_del(_splashSpinner);
        _splashSpinner = nullptr;
    }
    if (_splashLabel)
    {
        lv_obj_del(_splashLabel);
        _splashLabel = nullptr;
    }
    _splashFrame = 0;
    lv_obj_set_style_bg_color(lv_screen_active(), color565(BACKGROUND), 0);
    // A calibration modal outranks the view remembered before the splash: an
    // OTA attempt must not be a way to dismiss the lock screen.
    if (_calActive || _calShown)
    {
        showCalibrationView();
        return;
    }
    // Restore whichever view was up before splashScreen() hid it.
    if (_view == View::Chamber)
        showChamberView();
    else if (_view == View::Tool)
        showToolView();
    else if (_view == View::Bed)
        showBedView();
    else if (_view == View::Files)
        showFilesView();
    else
        showMainView();
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
    _filesContainer = createViewContainer(true);
    _infoContainer = createViewContainer(true);
    for (size_t slot = 0; slot < kPreheatCount; ++slot)
        _preheat[slot].container = createViewContainer(true);

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

    // The wifi-signal tap opens the info screen, handled on the MAIN CONTAINER
    // rather than on a dedicated hit-area object: one fewer LVGL object, which on
    // this board matters (see the note on the info view's pool budget).
    //
    // It has to be a coordinate test rather than "any tap on the container",
    // because LVGL bubbles events from the touched child up through its parents -
    // the chamber, bed, tool and file panels are all children of this container,
    // so an unfiltered handler would open the info screen every time the user
    // tapped a panel. mainContainerTapEvent() therefore checks where the touch
    // actually landed.
    lv_obj_add_flag(_mainContainer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_mainContainer, mainContainerTapEvent, LV_EVENT_PRESSED,
                        nullptr);

    _encLinkIcon = lv_image_create(_mainContainer);
    lv_image_set_src(_encLinkIcon, &icon_c3);
    lv_obj_set_size(_encLinkIcon, 16, 16);
    lv_obj_set_pos(_encLinkIcon, 252, 6);
    lv_obj_set_style_image_recolor(_encLinkIcon, color565(0x808080), 0);
    lv_obj_set_style_image_recolor_opa(_encLinkIcon, LV_OPA_COVER, 0);
    _encLinkUp = false;

    // HTTP enclosure-API fallback indicator. Left of the C3 icon, which shares
    // its row. Hidden unless the API is the live transport; the C3 icon already
    // reports freshness regardless of transport. Never recolored: its presence
    // IS the signal, so it is drawn green and shown/hidden rather than greyed
    // out (see refreshEnclosureLinkHeader).
    _apiLinkIcon = lv_image_create(_mainContainer);
    lv_image_set_src(_apiLinkIcon, &icon_api);
    lv_obj_set_size(_apiLinkIcon, 16, 16);
    lv_obj_set_pos(_apiLinkIcon, 230, 6);
    lv_obj_set_style_image_recolor(_apiLinkIcon, color565(TFT_GREEN), 0);
    lv_obj_set_style_image_recolor_opa(_apiLinkIcon, LV_OPA_COVER, 0);
    lv_obj_add_flag(_apiLinkIcon, LV_OBJ_FLAG_HIDDEN);
    _apiLinkUp = false;

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
    lv_obj_add_flag(_bedPanel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_bedPanel, bedPanelEvent, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(_toolPanel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_toolPanel, toolPanelEvent, LV_EVENT_CLICKED, nullptr);
    // Idle entry to the printable-files list. The tap handler itself checks
    // _jobActive, so no add/remove churn is needed on layout switches.
    lv_obj_add_flag(_filePanel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_filePanel, filesPanelEvent, LV_EVENT_CLICKED, nullptr);

    createPanelIcon(_toolPanel, &icon_nozzle);
    createPanelIcon(_bedPanel, &icon_bed);
    // Waves overlay: same parent and alignment as the body icon, so the two
    // draw as one. The body keeps its native orange; only the waves take the
    // temperature color (recolor at full opa replaces the pixel color fully,
    // alpha is kept - see recolor_only in lv_draw_sw_img.c).
    _bedWavesIcon = createPanelIcon(_bedPanel, &icon_bed_waves);
    lv_obj_set_style_image_recolor(_bedWavesIcon, color565(TFT_ORANGE), 0);
    lv_obj_set_style_image_recolor_opa(_bedWavesIcon, LV_OPA_COVER, 0);
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

    // One press sets the nozzle AND the bed together, which is what you actually
    // want before a print; a nozzle-only preheat just runs the bed cold. The
    // temperatures come from TOOL_PRESETS / BED_PRESETS (indexed by material),
    // so these buttons and the preheat screens can never disagree.
    const char *preheatLabels[] = {"PLA", "PETG", "ASA", "Off"};
    // Every one of these carries white text, so none of them can be a light
    // colour. TFT_YELLOW (0xFFFF00) in particular was unreadable on the CYD -
    // it is the brightest value the panel can show. PETG gets a dark gold that
    // still reads as "warm" but leaves enough contrast for white text.
    const uint32_t preheatColors[] = {TFT_ORANGE, 0xC88900, TFT_RED, TFT_BLUE};
    for (size_t index = 0; index < 4; ++index)
    {
        _preheatButtons[index] = lv_button_create(_mainContainer);
        lv_obj_set_size(_preheatButtons[index], kPreheatButtonWidth, 40);
        lv_obj_set_pos(_preheatButtons[index], preheatX[index], 180);
        styleButton(_preheatButtons[index], preheatColors[index]);
        lv_obj_add_event_cb(_preheatButtons[index], toolButtonEvent, LV_EVENT_CLICKED, reinterpret_cast<void *>(index));
        lv_obj_t *label = lv_label_create(_preheatButtons[index]);
        lv_label_set_text(label, preheatLabels[index]);
        // Explicit: the default label colour left "Off" looking dimmer than the
        // rest of the row.
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(label);
    }

    // Pause and Resume share the left slot (only one is ever visible), Cancel
    // owns the right slot. refreshJobButtons() owns this geometry from here
    // on; the sizes here only matter until the first job starts.
    const char *printLabels[] = {"Pause", "Cancel", "Resume"};
    const uint32_t printColors[] = {TFT_ORANGE, TFT_RED, TFT_DARKGREEN};
    const lv_coord_t printWidths[] = {150, 155, 150};
    const lv_coord_t printX[] = {5, 160, 5};
    for (size_t index = 0; index < 3; ++index)
    {
        _printButtons[index] = lv_button_create(_mainContainer);
        lv_obj_set_size(_printButtons[index], printWidths[index], 40);
        lv_obj_set_pos(_printButtons[index], printX[index], 180);
        styleButton(_printButtons[index], printColors[index]);
        lv_obj_add_event_cb(_printButtons[index], buttonEvent, LV_EVENT_CLICKED, reinterpret_cast<void *>(index));
        lv_obj_t *label = lv_label_create(_printButtons[index]);
        lv_label_set_text(label, printLabels[index]);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(label);
    }
    refreshJobButtons();
    setPreheatButtonsVisibility(true);
    createChamberView();
    createPreheatView(PreheatKind::Tool, "Nozzle Preheat");
    createPreheatView(PreheatKind::Bed, "Bed Preheat");
    createFilesView();
    createInfoView();
    createCalibrationView();
    createJobConfirm();
    showMainView();
    refreshChamberDetail();
    _isDrawn = true;
    setJobState(false, false);
    // lvglPoolFree is the pool's FREE bytes, not LV_MEM_SIZE.
//
// It used to print the pool SIZE, which is a constant and therefore says nothing
// about how much room is left - so the one number that matters when deciding
// whether a new view fits was missing, and an over-large one looked fine right
// up until lv_label_create() started returning NULL. free/total is what answers
// "is there room for this".
    lv_mem_monitor_t poolMon;
    lv_mem_monitor(&poolMon);
    Serial.printf("[HEAP] createLabes free=%u min=%u maxblock=%u lvglPool=%u/%u lvglTest=%d\n",
                  ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap(),
                  (unsigned)poolMon.free_size, (unsigned)poolMon.total_size,
                  (int)lv_mem_test());
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
                                                  stepperEvent, stepData(-CHAMBER_SETPOINT_STEP));
    _tempPlusButton = createChamberStepperButton(_tempContainer, 252, 4, "+", TFT_ORANGE,
                                                 stepperEvent, stepData(CHAMBER_SETPOINT_STEP));
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
                                                    louvreStepperEvent, stepData(-LOUVRE_STEP));
    _louvrePlusButton = createChamberStepperButton(_manContainer, 252, 4, "+", TFT_ORANGE,
                                                   louvreStepperEvent, stepData(LOUVRE_STEP));

    placeRowIcon(_manContainer, &icon_fan, 56);
    _fanLabel = lv_label_create(_manContainer);
    lv_label_set_text(_fanLabel, "Fan --");
    styleLabel(_fanLabel);
    lv_obj_set_pos(_fanLabel, 40, 59);
    _fanMinusButton = createChamberStepperButton(_manContainer, 186, 50, "-", TFT_BLUE,
                                                 fanStepperEvent, stepData(-FAN_STEP));
    _fanPlusButton = createChamberStepperButton(_manContainer, 252, 50, "+", TFT_ORANGE,
                                                fanStepperEvent, stepData(FAN_STEP));
}

int Display::preheatMax(PreheatKind kind)
{
    return kind == PreheatKind::Tool ? TOOL_TARGET_MAX : BED_TARGET_MAX;
}

int Display::preheatMin(PreheatKind kind)
{
    return kind == PreheatKind::Tool ? TOOL_TARGET_MIN : BED_TARGET_MIN;
}

const int *Display::preheatPresets(PreheatKind kind)
{
    return kind == PreheatKind::Tool ? TOOL_PRESETS : BED_PRESETS;
}

Display::View Display::preheatView(PreheatKind kind)
{
    return kind == PreheatKind::Tool ? View::Tool : View::Bed;
}

// One preheat screen template, instantiated once per target.
void Display::createPreheatView(PreheatKind kind, const char *title)
{
    const size_t slot = static_cast<size_t>(kind);
    PreheatView &view = _preheat[slot];
    lv_obj_t *parent = view.container;
    const int *presets = preheatPresets(kind);
    const int step = kind == PreheatKind::Tool ? TOOL_TARGET_STEP : BED_TARGET_STEP;
    view.target = kind == PreheatKind::Tool ? TOOL_TARGET_DEFAULT : BED_TARGET_DEFAULT;

    // Same shape as the chamber view: back button, title, then content.
    lv_obj_t *backButton = lv_button_create(parent);
    lv_obj_set_size(backButton, 100, 36);
    lv_obj_set_pos(backButton, 5, 5);
    styleButton(backButton, TFT_DARKGREY);
    lv_obj_add_event_cb(backButton, backEvent, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *backLabel = lv_label_create(backButton);
    lv_label_set_text(backLabel, LV_SYMBOL_LEFT " Back");
    lv_obj_remove_flag(backLabel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(backLabel);

    lv_obj_t *titleLabel = lv_label_create(parent);
    lv_label_set_text(titleLabel, title);
    styleLabel(titleLabel);
    lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 20, 12);

    // --- Material preset grid: 2 columns x 3 rows, mirroring the Material tab. ---
    const lv_coord_t gridX[] = {10, 165};
    const lv_coord_t gridY[] = {46, 82, 118};
    for (int index = 0; index < MATERIAL_COUNT; ++index)
    {
        view.presetButtons[index] = lv_button_create(parent);
        lv_obj_set_size(view.presetButtons[index], 145, 32);
        lv_obj_set_pos(view.presetButtons[index], gridX[index % 2], gridY[index / 2]);
        lv_obj_add_event_cb(view.presetButtons[index], preheatPresetEvent, LV_EVENT_CLICKED,
                            addPreheatBinding(kind, index));
        lv_obj_t *label = lv_label_create(view.presetButtons[index]);
        lv_label_set_text(label, (String(MATERIAL_PRESETS[index].name) + " " +
                                  String(presets[index]) + "C")
                                     .c_str());
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(label);
    }

    // --- Custom stepper row. ---
    view.currentLabel = lv_label_create(parent);
    lv_label_set_text(view.currentLabel, "-- C");
    styleLabel(view.currentLabel);
    lv_obj_set_pos(view.currentLabel, 10, 158);
    view.targetLabel = lv_label_create(parent);
    lv_label_set_text(view.targetLabel, "-- C");
    lv_obj_set_style_text_color(view.targetLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(view.targetLabel, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(view.targetLabel, 10, 176);
    view.minusButton = createChamberStepperButton(parent, 186, 158, "-", TFT_BLUE,
                                                  preheatStepperEvent,
                                                  addPreheatBinding(kind, -step));
    view.plusButton = createChamberStepperButton(parent, 252, 158, "+", TFT_ORANGE,
                                                 preheatStepperEvent,
                                                 addPreheatBinding(kind, step));

    // Off needs its own button: 0 is the value a step-down would take a long
    // time to reach, and it is the one action with real wait behind it.
    view.offButton = lv_button_create(parent);
    lv_obj_set_size(view.offButton, 145, 32);
    lv_obj_set_pos(view.offButton, 10, 202);
    styleButton(view.offButton, TFT_BLUE);
    lv_obj_add_event_cb(view.offButton, preheatOffEvent, LV_EVENT_CLICKED,
                        addPreheatBinding(kind, 0));
    lv_obj_t *offLabel = lv_label_create(view.offButton);
    lv_label_set_text(offLabel, "Off");
    lv_obj_remove_flag(offLabel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(offLabel);

    setPreheatControlsEnabled(kind, !_jobActive);
}

void Display::setPreheatTarget(PreheatKind kind, int temp)
{
    PreheatView &view = _preheat[static_cast<size_t>(kind)];
    // Changing a target mid-print is almost always a slip, and OctoPrint will
    // happily accept it. The controls are also greyed out, but guard here too:
    // _jobActive is the authority, not the visual state.
    if (_jobActive)
    {
        Serial.printf("[PREHEAT] kind=%d REJECTED %dC jobActive\n", (int)kind, temp);
        return;
    }
    view.target = constrain(temp, preheatMin(kind), preheatMax(kind));
    Serial.printf("[PREHEAT] kind=%d set %dC action=%p\n", (int)kind, view.target,
                  (void *)_preheatAction[static_cast<size_t>(kind)]);
    if (_preheatAction[static_cast<size_t>(kind)])
    {
        view.sentMs = millis();
        _preheatAction[static_cast<size_t>(kind)](view.target);
    }
    refreshPreheatDetail(kind);
}

void Display::selectPreheatPreset(PreheatKind kind, int index)
{
    if (index < 0 || index >= MATERIAL_COUNT)
        return;
    setPreheatTarget(kind, preheatPresets(kind)[index]);
}

void Display::stepPreheatTarget(PreheatKind kind, int delta)
{
    if (_jobActive)
        return;
    PreheatView &view = _preheat[static_cast<size_t>(kind)];
    // Step from the live target when there is one, so the first tap off a cold
    // target lands on a sane value instead of one step from the default.
    int base = view.target;
    if (base == 0)
        base = kind == PreheatKind::Tool ? TOOL_TARGET_DEFAULT : BED_TARGET_DEFAULT;
    setPreheatTarget(kind, base + delta);
}

void Display::setPreheatControlsEnabled(PreheatKind kind, bool enabled)
{
    PreheatView &view = _preheat[static_cast<size_t>(kind)];
    if (view.controlsEnabled == enabled)
        return;
    view.controlsEnabled = enabled;
    // Clearing CLICKABLE is what actually stops the taps; the opacity change is
    // only so it is visible that they are inert.
    lv_obj_t *controls[MATERIAL_COUNT + 3];
    for (int index = 0; index < MATERIAL_COUNT; ++index)
        controls[index] = view.presetButtons[index];
    controls[MATERIAL_COUNT] = view.offButton;
    controls[MATERIAL_COUNT + 1] = view.minusButton;
    controls[MATERIAL_COUNT + 2] = view.plusButton;
    for (lv_obj_t *control : controls)
    {
        if (!control)
            continue;
        if (enabled)
        {
            lv_obj_add_flag(control, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_opa(control, LV_OPA_COVER, 0);
        }
        else
        {
            lv_obj_clear_flag(control, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_opa(control, LV_OPA_50, 0);
        }
    }
    if (view.targetLabel)
        lv_obj_set_style_text_opa(view.targetLabel, enabled ? LV_OPA_COVER : LV_OPA_50, 0);
}

void Display::refreshPreheatDetail(PreheatKind kind)
{
    const PreheatView &view = _preheat[static_cast<size_t>(kind)];
    if (!view.targetLabel)
        return;
    if (view.target == 0)
        setLabelTextOnce(view.targetLabel, "Off");
    else
        setLabelTextOnce(view.targetLabel, String(view.target) + " C");
    if (view.currentLabel && view.actualValid)
        setLabelTextOnce(view.currentLabel, String(view.actual, 1) + " C");
}

// Hides every view container. The single place that knows the full list, so a
// new view cannot be added to some hide-lists and missed in others.
//
// Null-safe throughout: this is also called from the splash path during boot,
// before createLabes() has created anything.
// ---------------------------------------------------------------------------
// Info screen
//
// ONE multi-line label, not one per row. The first version used a label and a
// value per row (42 objects), which exhausted the 60 KB LVGL pool -
// lv_label_create() began returning NULL and the board boot-looped. Values are
// space-padded into the same string as their label instead, so alignment is
// approximate (montserrat is not monospace) and the screen costs one object.
// That is a deliberate trade for the board booting at all.
//
// TWELVE lines in THREE categories, separated by blank lines. The screen answers
// three unrelated questions - what am I, can I reach the fan board, can I reach
// the printer - and running them together as seven undifferentiated rows made it
// read as a wall of text. Twelve 16 px lines under the title land at 212 px of
// 240.
//
//   THIS BOARD - Firmware / Uptime / Address. What this board is.
//   C3         - Link / Status.           Whether the fan board is reachable.
//   OCTOPRINT  - Address / Status.        Whether the printer server is up.
//
// The blank separators cost no LVGL objects at all: extra newlines in one label
// are extra blank lines, not extra widgets. That matters - see the pool note
// above, where the object count is the binding constraint on this screen.
//
// Each category's two facts are ADDRESS and STATUS as separate rows rather than
// one, because their lifetimes differ: an address is fixed at boot and never
// changes, while the status is the thing you actually opened the screen to
// check. Merged, the colour would end up reporting on the address.
//
// Headings and status values are COLOUR-CODED through LVGL's inline recolor
// markup (lv_label_set_recolor) rather than by giving each its own label. A
// second label per coloured row would be five more LVGL objects - the exact thing
// that exhausted the pool here before - and recolor costs zero.
//
// Deliberately NOT on this screen: packet counters, drop reasons, BSSID,
// channel, MACs, RSSI, and the C3's own IP. All of the diagnostics are on the
// serial log at a 1 Hz heartbeat, which is where a number you need to compare
// frame by frame belongs. A screen you tap while checking whether something works
// should answer that in one look, not make you read twelve lines to find out.
//
// The MAC/BSSID pairing facts are not lost, just moved: the boot pairing screen
// shows them when you are actually transcribing one into another board.
//
// Values are formatted into char buffers and handed to the label, never
// assembled as Strings. This board's static DRAM has historically been ~800
// bytes under its linker limit, and a diagnostic screen refreshed every tick is
// exactly the place a per-frame String would quietly cost more than it looks.
// ---------------------------------------------------------------------------

// 16 px IS montserrat_14's own line height (lv_font_montserrat_14.c:
// .line_height = 16), so rows butt up exactly as the font intends - there is no
// descender saving being claimed here.
static constexpr lv_coord_t kInfoLineH = 16;
static constexpr lv_coord_t kInfoTop = 20; // under the title/back row
static constexpr lv_coord_t kInfoLeftX = 2;
// Rows the single block writes. Load-bearing: kInfoRows rows at kInfoLineH must
// fit SCREEN_HEIGHT - kInfoTop, and this is the number to change when adding or
// removing one.
//
// The count includes the BLANK separator lines between the three categories, not
// just the data rows: 3 headers + 7 data rows + 2 blanks = 12. 12 * 16 + 20 =
// 212 of 240 px.
static constexpr size_t kInfoRows = 12;

// Recolor command for lv_label_set_recolor(): '#', six hex digits, a space, the
// text, then '#' again. The parser (lv_draw_label.c RECOLOR_CMD_STATE_*) consumes
// all of it and never draws it - but it DOES consume buffer, which is why
// kInfoBuf has to allow for it. A bare '#' inside a value is escaped by doubling
// it ("##"), so a value containing one cannot be used here unescaped.
#define INFO_RECOLOR_GREEN "#00FF00 "
#define INFO_RECOLOR_RED "#FF4040 "
// Amber for the three category headings: distinct from both the white data rows
// and the green/red status values, so a heading never reads as a status.
#define INFO_RECOLOR_HEAD "#FFB020 "
#define INFO_RECOLOR_END "#"

// Pixel x at which every value starts, so the column lines up on a real boundary
// rather than on a character count.
//
// Montserrat is proportional: "WiFi" and "www" are four characters and four very
// different widths, so the old "%-6s" padding lined the values up to WITHIN a few
// pixels of each other and no further. That is why it looked ragged.
//
// The label is padded with spaces until MEASURED pixel width reaches this value.
// The width comes from lv_font_get_glyph_width(), so it stays correct if the font
// or its size ever changes - the alternative, hardcoding a character count, is
// exactly the thing that drifts.
//
// Sized to the WIDEST label on the screen ("Firmware" = 70 px), not to a round
// number: any label wider than this is dropped by infoPadLabel rather than
// allowed to push its value out of the column.
//
// Grouping the rows into three categories let the labels shrink from
// "Printer status" (99 px) to "Firmware" (70 px), which is what brought the
// column back from 104 px to 78 - 26 px more room for every value, and the
// widest value ("255.255.255.255:65535" = 149 px) still ends 87 px inside the
// 314 px label.
static constexpr lv_coord_t kInfoValueX = 78;

// Width of `text` in pixels at the info font. Per-glyph, and LVGL applies
// kerning via letter_next, so it matches what the label will actually draw.
//
// Only valid for the ASCII the rows use. A multi-byte character would be measured
// per byte and come out wrong - acceptable here because every label and value on
// this screen is ASCII, and a wrong-looking row is not a crash.
static lv_coord_t infoTextWidth(const char *text)
{
    lv_coord_t width = 0;
    for (const char *p = text; *p; ++p)
        width += lv_font_get_glyph_width(&lv_font_montserrat_14, (uint32_t)(uint8_t)*p, 0);
    return width;
}

// Copies `label` into `out` and pads it with spaces until it is at least
// kInfoValueX pixels wide. Shared by both row writers so a plain row and a
// coloured one can never drift out of alignment with each other.
//
// Returns false if the label is too long to pad into `out` - in which case the
// caller drops the row. A label wider than the column would silently push its
// value right and break the alignment of every row below it.
static bool infoPadLabel(char *out, size_t cap, const char *label)
{
    size_t len = 0;
    for (const char *p = label; *p && len + 1 < cap; ++p)
        out[len++] = *p;
    out[len] = '\0';

    // Track the width in a VARIABLE rather than re-measuring `out` each time.
    //
    // The obvious loop - "while (infoTextWidth(out) < kInfoValueX) out[len++] =
    // ' ';" - is WRONG, and was: it appends a space and then measures a buffer
    // whose NUL is still one position behind, so infoTextWidth() runs on past the
    // end into uninitialized stack until it stumbles on a zero byte. The pad
    // count then depended on whatever was on the stack that refresh, which is
    // why the column visibly WOBBLED between updates instead of being
    // consistently off.
    lv_coord_t width = infoTextWidth(out);

    // A label already at or past the column cannot be padded back, so it would
    // overflow the column. Reject it rather than misalign the screen.
    if (width >= kInfoValueX)
        return false;

    const lv_coord_t spaceW =
        lv_font_get_glyph_width(&lv_font_montserrat_14, (uint32_t)' ', 0);
    while (width + spaceW <= kInfoValueX && len + 1 < cap)
    {
        out[len++] = ' ';
        width += spaceW;
    }
    out[len] = '\0';
    return true;
}

// Appends one "label<pad>value" line, padding the label out to kInfoValueX.
static void infoLine(char *buf, size_t n, const char *label, const char *value)
{
    // +2 for the '\n' and the NUL that snprintf always writes.
    const size_t used = strnlen(buf, n);
    if (used + 2 >= n)
        return; // buffer full: drop the row rather than truncate mid-value
    char padded[32];
    if (!infoPadLabel(padded, sizeof(padded), label))
        return;
    snprintf(buf + used, n - used, "%s%s%s\n", used ? "" : "", padded,
             value ? value : "-");
}

// As infoLine, but the VALUE is wrapped in an LVGL recolor command so a single
// label can carry a red/green status. This is what makes the traffic-light rows
// possible without a second LVGL object per row - the whole reason the screen
// fits in the pool at all.
//
// Requires lv_label_set_recolor(_infoLabel, true), set in createInfoView().
// text must NOT contain a bare '#' (see INFO_RECOLOR_END).
static void infoStatusLine(char *buf, size_t n, const char *label,
                           const char *color, const char *value)
{
    const size_t used = strnlen(buf, n);
    // 2 for the '\n' and the NUL, plus the recolor wrapper is accounted for by
    // the caller sizing kInfoBuf generously.
    if (used + 2 >= n)
        return;
    char padded[32];
    if (!infoPadLabel(padded, sizeof(padded), label))
        return;
    snprintf(buf + used, n - used, "%s%s%s%s" INFO_RECOLOR_END "\n",
             used ? "" : "", padded, color, value ? value : "-");
}

static void infoBlockStart(char *buf, size_t n)
{
    buf[0] = '\0';
}

// Appends a category heading - NOT padded to the value column, because it is a
// title for the rows under it, not a label in the table.
//
// Coloured so the three groups read as three groups even at a glance, and so the
// white rows below them are visibly the data.
static void infoHeader(char *buf, size_t n, const char *text)
{
    const size_t used = strnlen(buf, n);
    if (used + 2 >= n)
        return;
    snprintf(buf + used, n - used, "%s" INFO_RECOLOR_HEAD "%s" INFO_RECOLOR_END
             "\n", used ? "" : "", text);
}

// Appends a BLANK line, which is what separates one category from the next.
//
// Cheaper than it looks: a label with N rows and N-1 newlines renders the same
// as one with extra empty lines between them, so grouping costs no extra LVGL
// objects - only a few bytes of buffer and 16 px of vertical space each.
static void infoGap(char *buf, size_t n)
{
    const size_t used = strnlen(buf, n);
    if (used + 2 >= n)
        return;
    snprintf(buf + used, n - used, "\n");
}

void Display::createInfoView()
{
    lv_obj_t *parent = _infoContainer;

    // No title label: the Back button already labels the screen. One fewer
    // object for a word that changes never.
    lv_obj_t *backButton = lv_button_create(parent);
    lv_obj_set_size(backButton, 56, 18);
    lv_obj_set_pos(backButton, SCREEN_WIDTH - 62, 0);
    lv_obj_add_flag(backButton, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(backButton, infoBackEvent, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *backLabel = lv_label_create(backButton);
    if (backLabel)
    {
        lv_label_set_text(backLabel, "Back");
        lv_obj_set_style_text_font(backLabel, &lv_font_montserrat_14, 0);
        lv_obj_center(backLabel);
    }

    // ONE label for the whole screen. The allocation is guarded: an exhausted
    // pool returns NULL, and handing that to lv_label_set_text() is what
    // boot-looped this board before. A screen that comes up short is a cosmetic
    // problem; a board that reboots forever is not.
    _infoLabel = lv_label_create(parent);
    if (!_infoLabel)
    {
        Serial.println("[UI] info view: LVGL pool exhausted, label missing");
    }
    else
    {
        lv_obj_set_pos(_infoLabel, kInfoLeftX, kInfoTop);
        lv_obj_set_width(_infoLabel, SCREEN_WIDTH - kInfoLeftX - 4);
        lv_obj_set_style_text_color(_infoLabel, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(_infoLabel, &lv_font_montserrat_14, 0);
        // WRAP, not DOT: the rows are explicit '\n'-separated, and a long value
        // (a BSSID, a MAC) must stay readable rather than being ellipsised away -
        // comparing those by eye is the point of the screen.
        lv_label_set_long_mode(_infoLabel, LV_LABEL_LONG_WRAP);
        // Lets a single label carry red/green status text via the '#RRGGBB ..#'
        // markup. Costs no extra LVGL object, which is the point - see the note
        // on the info view's pool budget.
        lv_label_set_recolor(_infoLabel, true);
        lv_label_set_text(_infoLabel, "");
    }

    // Tap-anywhere-to-close, on the container itself. Attached once at creation
    // and gated on _view inside the handler (see infoTapEvent) rather than
    // added/removed on entry and exit, so it can never be left attached and fire
    // against another view.
    lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(parent, infoTapEvent, LV_EVENT_CLICKED, nullptr);
}

void Display::createCalibrationView()
{
    _calContainer = createViewContainer(true);

    _calTitleLabel = lv_label_create(_calContainer);
    lv_label_set_text(_calTitleLabel, "PID Calibration");
    lv_obj_set_style_text_color(_calTitleLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(_calTitleLabel, &lv_font_montserrat_20, 0);
    lv_obj_align(_calTitleLabel, LV_ALIGN_TOP_MID, 0, 8);

    // ONE label for every reading. There is no reason to allocate a widget per
    // row here, and this board has boot-looped on pool exhaustion before (see
    // the info view).
    _calBodyLabel = lv_label_create(_calContainer);
    if (_calBodyLabel)
    {
        lv_obj_set_pos(_calBodyLabel, 12, 38);
        lv_obj_set_width(_calBodyLabel, SCREEN_WIDTH - 24);
        lv_obj_set_style_text_color(_calBodyLabel, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(_calBodyLabel, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(_calBodyLabel, LV_LABEL_LONG_WRAP);
        lv_label_set_text(_calBodyLabel, "");
    }
    else
    {
        Serial.println("[UI] calibration view: LVGL pool exhausted, label missing");
    }

    _calButton = lv_button_create(_calContainer);
    lv_obj_set_size(_calButton, 190, 40);
    lv_obj_align(_calButton, LV_ALIGN_BOTTOM_MID, 0, -8);
    styleButton(_calButton, TFT_RED);
    lv_obj_add_event_cb(_calButton, calibrationButtonEvent, LV_EVENT_CLICKED, nullptr);
    _calButtonLabel = lv_label_create(_calButton);
    if (_calButtonLabel)
    {
        lv_label_set_text(_calButtonLabel, "Cancel calibration");
        lv_obj_set_style_text_color(_calButtonLabel, lv_color_hex(0xFFFFFF), 0);
        lv_obj_remove_flag(_calButtonLabel, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(_calButtonLabel);
    }
}

void Display::showCalibrationView()
{
    if (!_calContainer)
        return;
    _calShown = true;
    _view = View::Calibration;
    refreshCalibrationDetail();
    hideAllViews();
    lv_obj_clear_flag(_calContainer, LV_OBJ_FLAG_HIDDEN);
}

void Display::showInfoView()
{
    if (!_mainContainer || !_infoContainer)
        return;
    // The calibration modal owns the screen for the duration of the run. This
    // is a belt-and-braces guard: its container already covers the header, so
    // no tap should reach here, but every navigation path must agree rather
    // than rely on what happens to be visible.
    if (_calActive)
    {
        showCalibrationView();
        return;
    }
    _view = View::Info;
    infoUpdate();
    hideAllViews();
    lv_obj_clear_flag(_infoContainer, LV_OBJ_FLAG_HIDDEN);
}
// "3d 04h", "4h 12m", "12m 03s" - coarse on purpose, so the width never jumps
// between refreshes and pushes the value around.
static void formatUptime(char *buf, size_t n)
{
    const unsigned long up = millis();
    const unsigned long secs = up / 1000UL;
    if (secs >= 86400UL)
        snprintf(buf, n, "%lu d %02lu h", (unsigned long)(secs / 86400UL),
                 (unsigned long)((secs % 86400UL) / 3600UL));
    else if (secs >= 3600UL)
        snprintf(buf, n, "%lu h %02lu m", (unsigned long)(secs / 3600UL),
                 (unsigned long)((secs % 3600UL) / 60UL));
    else
        snprintf(buf, n, "%lu m %02lu s", (unsigned long)(secs / 60UL),
                 (unsigned long)(secs % 60UL));
}

// millis() wraps every ~49.7 days. Ages are computed with a SIGNED difference so
// a wrapped clock reads as "just now" rather than as a 49-day-old reading, which
// is the failure the subtraction-order rule exists to prevent.
static unsigned long ageMs(unsigned long since)
{
    return since == 0 ? 0UL : (unsigned long)((int32_t)(millis() - since));
}

// Dotted-quad for either board's address. IPAddress deliberately gets its own
// rather than reusing a MAC formatter: it is FOUR octets, and the MAC helper
// reads six - so passing it one would read two bytes past the object and print a
// plausible-looking trailing pair.
static void infoAddr(const IPAddress &ip, char *buf, size_t n)
{
    snprintf(buf, n, "%u.%u.%u.%u", (unsigned)ip[0], (unsigned)ip[1],
             (unsigned)ip[2], (unsigned)ip[3]);
}

// How stale an OctoPrint reading may get before the row says Offline.
//
// Ten poll cycles at API_REFRESH_TIME (3 s). Generous on purpose: OctoPrint is
// slow to answer while a large slice is being requested, and a row that flickers
// red mid-print is worse than one that takes a few seconds to admit a real
// outage. It is still well inside the window where "unreachable" and "idle" have
// to be told apart, which is the whole reason this threshold exists.
static constexpr unsigned long kOctoStaleMs = 30000UL;

void Display::infoUpdate()
{
    // Cheap no-op unless this screen is up. Called from the UI tick, so without
    // this it would repaint labels behind whatever view the user is actually on.
    if (_view != View::Info || !_infoContainer)
        return;

    // Long enough for the widest value on the screen ("255.255.255.255:65535" = 21),
    // plus headroom so a longer hostname or version string is never truncated -
    // a truncated status row reads as a real value, which is worse than a short
    // one.
    char value[48];
    PollerOctoInfo octo = {};
    const bool haveOcto = pollerCopyOctoInfo(octo);
    const EnclosureLinkStats link = enclosureLinkStats();

    infoBlockStart(_infoText, sizeof(_infoText));

    // Three CATEGORIES, separated by a blank line. The screen answers three
    // unrelated questions - what am I, can I reach the fan board, can I reach the
    // printer - and running them together as seven undifferentiated rows made it
    // read as a wall. Grouping them costs two blank lines and no extra objects.
    //
    // Grouping is also what bought the narrower label column: inside a group the
    // label can be the short form ("Address", not "This board" / "OctoPrint"),
    // which drops the widest label from 99 px to 70 px.
    infoHeader(_infoText, sizeof(_infoText), "THIS BOARD");
    snprintf(value, sizeof(value), "v" ENC_FW_VERSION_STR " proto %d",
             (int)ENC_PROTO_VERSION);
    infoLine(_infoText, sizeof(_infoText), "Firmware", value);
    formatUptime(value, sizeof(value));
    infoLine(_infoText, sizeof(_infoText), "Uptime", value);
    infoAddr(WiFi.localIP(), value, sizeof(value));
    infoLine(_infoText, sizeof(_infoText), "Address", value);

    infoGap(_infoText, sizeof(_infoText));
    infoHeader(_infoText, sizeof(_infoText), "C3  (enclosure fan board)");

    // Two rows, because "which transport" and "is it working" are separate
    // questions and merging them hides one when the other is fine. The counters
    // and drop reasons are gone: they are on the serial log at 1 Hz, which is
    // where you compare them frame by frame.
    const bool linkUp = link.apiActive || link.lastRxMs != 0;
    infoLine(_infoText, sizeof(_infoText), "Link",
             linkUp ? (link.apiActive ? "HTTP API" : "ESP-NOW radio")
                    : "not connected");
    infoStatusLine(_infoText, sizeof(_infoText), "Status",
                   linkUp ? INFO_RECOLOR_GREEN : INFO_RECOLOR_RED,
                   linkUp ? "online" : "offline");

    infoGap(_infoText, sizeof(_infoText));
    infoHeader(_infoText, sizeof(_infoText), "OCTOPRINT");

    // Same split as the C3 group: the address never changes but the reachability
    // does, and a single row forces one colour onto both facts.
    //
    // Offline is decided by FRESHNESS, not by the poll's own valid flag: the poll
    // task keeps republishing the last good values after OctoPrint stops
    // answering, so a printer that has gone away still looks perfectly fine
    // without this check. The threshold is generous on purpose - three missed
    // polls on a busy print (API_REFRESH_TIME is 3 s) should not flicker the row
    // red mid-job.
    const unsigned long octoAge = ageMs(haveOcto ? octo.lastUpdateMs : 0);
    const bool octoOnline =
        haveOcto && octo.valid && octo.lastUpdateMs != 0 &&
        octoAge < kOctoStaleMs;
    if (haveOcto && octo.host.toString().length())
    {
        const String host = octo.host.toString();
        snprintf(value, sizeof(value), "%s:%u", host.c_str(), (unsigned)octo.port);
    }
    else
    {
        snprintf(value, sizeof(value), "not configured");
    }
    infoLine(_infoText, sizeof(_infoText), "Address", value);
    infoStatusLine(_infoText, sizeof(_infoText), "Status",
                   octoOnline ? INFO_RECOLOR_GREEN : INFO_RECOLOR_RED,
                   octoOnline ? "online" : "offline");

    // ONE set_text for the whole screen, not one per row: twelve lines, one call.
    if (_infoLabel)
        lv_label_set_text(_infoLabel, _infoText);
}

void Display::hideAllViews()
{
    if (_mainContainer)
        lv_obj_add_flag(_mainContainer, LV_OBJ_FLAG_HIDDEN);
    if (_chamberContainer)
        lv_obj_add_flag(_chamberContainer, LV_OBJ_FLAG_HIDDEN);
    if (_filesContainer)
        lv_obj_add_flag(_filesContainer, LV_OBJ_FLAG_HIDDEN);
    if (_infoContainer)
        lv_obj_add_flag(_infoContainer, LV_OBJ_FLAG_HIDDEN);
    if (_calContainer)
        lv_obj_add_flag(_calContainer, LV_OBJ_FLAG_HIDDEN);
    for (size_t slot = 0; slot < kPreheatCount; ++slot)
        if (_preheat[slot].container)
            lv_obj_add_flag(_preheat[slot].container, LV_OBJ_FLAG_HIDDEN);
}

void Display::showMainView()
{
    if (_calActive)
    {
        showCalibrationView();
        return;
    }
    _view = View::Main;
    hideAllViews();
    if (_mainContainer)
        lv_obj_clear_flag(_mainContainer, LV_OBJ_FLAG_HIDDEN);
}

void Display::showToolView()
{
    showPreheatView(PreheatKind::Tool);
}

void Display::showBedView()
{
    showPreheatView(PreheatKind::Bed);
}

void Display::showPreheatView(PreheatKind kind)
{
    if (_calActive)
    {
        showCalibrationView();
        return;
    }
    const size_t slot = static_cast<size_t>(kind);
    if (!_mainContainer || !_preheat[slot].container)
        return;
    _view = preheatView(kind);
    refreshPreheatDetail(kind);
    hideAllViews();
    lv_obj_clear_flag(_preheat[slot].container, LV_OBJ_FLAG_HIDDEN);
}

void Display::showChamberView()
{
    if (_calActive)
    {
        showCalibrationView();
        return;
    }
    if (!_mainContainer || !_chamberContainer)
        return;
    _view = View::Chamber;
    refreshChamberDetail();
    hideAllViews();
    lv_obj_clear_flag(_chamberContainer, LV_OBJ_FLAG_HIDDEN);
    Serial.printf("[HEAP] chamber open free=%u min=%u maxblock=%u lvglTest=%d\n",
                  ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap(),
                  (int)lv_mem_test());
}

// Same shape as the chamber/preheat views: back button, title, then content.
// Row buttons are a fixed pool created once and recycled by
// refreshFilesDetail(), so opening the list never mallocs in steady state.
// Rows in the file-info table: the file name, then one row per fact. NO header
// row - the name IS the first row, which is what the screen is about.
static const int kFileInfoRows = 10;

// Lines of text drawn in each file row: the file name, then a compact detail
// line (material, print time, size). Two is what fits the 46 px row at font 14
// with the padding the centring calculation assumes.
static const int kFileRowTextLines = 2;

void Display::createFilesView()
{
    lv_obj_t *parent = _filesContainer;

    lv_obj_t *backButton = lv_button_create(parent);
    lv_obj_set_size(backButton, 100, 32);
    lv_obj_set_pos(backButton, 5, 5);
    styleButton(backButton, TFT_DARKGREY);
    lv_obj_add_event_cb(backButton, backEvent, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *backLabel = lv_label_create(backButton);
    lv_label_set_text(backLabel, LV_SYMBOL_LEFT " Back");
    lv_obj_remove_flag(backLabel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(backLabel);

    lv_obj_t *titleLabel = lv_label_create(parent);
    lv_label_set_text(titleLabel, "Files");
    styleLabel(titleLabel);
    lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 20, 12);

    _filesStatusLabel = lv_label_create(parent);
    lv_label_set_text(_filesStatusLabel, "Loading...");
    styleLabel(_filesStatusLabel);
    lv_obj_set_width(_filesStatusLabel, 310);
    lv_label_set_long_mode(_filesStatusLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(_filesStatusLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(_filesStatusLabel, 5, 39);

    // Only this container scrolls; every other view stays fixed (see
    // createViewContainer). Vertical drag scrolls the rows on touch.
    // Four rows fit by geometry, not by a setting: 4 rows x 36 px pitch must fit the
// 144 px box below. Asserted so a future change to the pitch or the box fails the
// build instead of quietly sliding the last visible row under the print button.
static_assert(PRINT_FILES_VISIBLE * 48 <= 146,
              "files list box must fit PRINT_FILES_VISIBLE rows at the row pitch below");

// 146 px is exactly what three 46 px rows need at a 48 px pitch: the last one
    // starts at 2 + 2*48 = 98 and ends at 144.
    //
    // Dropping from four visible rows to three is what makes them feel properly
    // large: 46 px of row for a 24 px line box is room to breathe, and a tap
    // target you cannot miss. Cramming four in gave 34 px, which read as cramped
    // on a touch panel this size.
    //
    // The box holds three; MAX_PRINT_FILES (ten) rows are created below and run
    // past its bottom edge, which is what makes the list scroll.
    _filesList = lv_obj_create(parent);
lv_obj_set_pos(_filesList, 0, 56);
    lv_obj_set_size(_filesList, SCREEN_WIDTH, 146);
    lv_obj_set_style_bg_color(_filesList, color565(BACKGROUND), 0);
    lv_obj_set_style_border_width(_filesList, 0, 0);
    lv_obj_set_style_radius(_filesList, 0, 0);
    lv_obj_set_style_pad_all(_filesList, 0, 0);
    lv_obj_add_flag(_filesList, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(_filesList, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_filesList, LV_SCROLLBAR_MODE_AUTO);

    // Centred from the FONT's real line height rather than a hand-picked pad_top.
// A guessed number was wrong here: it did not centre the text, and it would have
// to be redone by hand whenever the font or the row height changed. Asking the
// font makes the padding follow both.
const lv_coord_t lineHeight = lv_font_get_line_height(&lv_font_montserrat_14);
    // Two lines per row (name, then a detail line), so the block is twice the
    // line height and the padding is what centres THAT, not a single line.
    // kFileRowTextLines exists so the two can never disagree.
    const lv_coord_t rowPadTop = (46 - kFileRowTextLines * lineHeight) / 2;

    for (int index = 0; index < MAX_PRINT_FILES; ++index)
    {
        // The row IS the label: a button + label per row (40 objects) blew
        // the 60 KB LVGL pool and crashed the next create in createLabes.
        // Labels take CLICKED events once flagged clickable, and styleButton
        // gives them the same row background + selection recolor. NOTE:
        // labels default to a TRANSPARENT background, so the bg color alone
        // is invisible - bg_opa COVER is what makes selected/unselected
        // actually differ on screen. One rolling line per row (name only).
        lv_obj_t *row = lv_label_create(_filesList);
        // montserrat_14, deliberately NOT the 20 px font the rest of the panels use.
        // Width is the constraint here, not height: the row is 305 px wide and a
        // gcode filename is long, so at 20 px only a dozen or so characters fit
        // before the circular-scroll takes over and the name is unreadable until
        // you wait for it to slide. At 14 px roughly 1.7x more of the name is on
        // screen at once, which is the whole point of showing a file list.
        //
        // The row stays 46 px tall, so the smaller text costs nothing in touch
        // target - it is centred in the row with ~14 px above it.
        lv_obj_set_style_text_font(row, &lv_font_montserrat_14, 0);
        lv_obj_set_size(row, 305, 46);
        lv_obj_set_pos(row, 5, 2 + index * 48);
        styleButton(row, TFT_DARKGREY);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(row, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_pad_left(row, 8, 0);
        lv_obj_set_style_pad_right(row, 8, 0);
        lv_obj_set_style_pad_top(row, rowPadTop, 0);
        lv_obj_set_width(row, 305);
        lv_label_set_long_mode(row, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_label_set_text(row, "");
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, fileRowEvent, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(index)));
        _fileRowLabels[index] = row;
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }

    _filesPrintButton = lv_button_create(parent);
lv_obj_set_size(_filesPrintButton, 150, 32);
    lv_obj_set_pos(_filesPrintButton, 165, 204);
    styleButton(_filesPrintButton, TFT_DARKGREEN);
    lv_obj_add_event_cb(_filesPrintButton, filesPrintEvent, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *printLabel = lv_label_create(_filesPrintButton);
    lv_label_set_text(printLabel, "Print file");
    lv_obj_set_style_text_color(printLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_remove_flag(printLabel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(printLabel);
    // Always visible; refreshFilesDetail() owns enabled vs. disabled state
    // (disabled = greyed and not clickable until a row is selected).

    // Info sits BESIDE Print rather than replacing it: both act on the selected
    // row, and making the details a round trip away from the start button would
    // mean going back to print afterwards. Same enable/disable rule.
    _filesInfoButton = lv_button_create(parent);
    lv_obj_set_size(_filesInfoButton, 150, 32);
    lv_obj_set_pos(_filesInfoButton, 5, 204);
    styleButton(_filesInfoButton, TFT_BLUE);
    lv_obj_add_event_cb(_filesInfoButton, filesInfoEvent, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *infoLabel = lv_label_create(_filesInfoButton);
    lv_label_set_text(infoLabel, "File info");
    lv_obj_set_style_text_color(infoLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_remove_flag(infoLabel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(infoLabel);

    // The details panel replaces the list in place, rather than being its own
    // view with its own container - one name label and one table is all it needs,
    // and every LVGL object counts against the 60 KB pool.
    //
    // NO back button: a touch anywhere returns to the list instead (see
    // filesPanelTouchEvent). A corner button would compete with the table for
    // the same panel and, on a 320-wide screen, eat a column of values.
    lv_obj_add_event_cb(parent, filesPanelTouchEvent, LV_EVENT_PRESSED, nullptr);


    // A real two-column table rather than a wall of text: the whole point of
    // looking something up is being able to scan the values, which a block of
    // prose cannot do. LVGL 9 exposes only LV_PART_MAIN and LV_PART_ITEMS, so
    // per-column styling is done by reaching the cell objects directly (they are
    // the table's children, in row-major order).
    _filesInfoTable = lv_table_create(parent);
    lv_table_set_column_count(_filesInfoTable, 2);
    lv_table_set_row_count(_filesInfoTable, kFileInfoRows);
    lv_table_set_column_width(_filesInfoTable, 0, 118);
    lv_table_set_column_width(_filesInfoTable, 1, 187);
    // The table IS the screen. With the back button gone there is nothing else
    // on the panel, so it runs from just inside the top edge to just inside the
    // bottom, and 10 rows fit without scrolling.
    lv_obj_set_pos(_filesInfoTable, 5, 5);
    lv_obj_set_size(_filesInfoTable, 310, 230);
    lv_obj_set_style_bg_color(_filesInfoTable, color565(BACKGROUND), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_filesInfoTable, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(_filesInfoTable, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(_filesInfoTable, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(_filesInfoTable, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(_filesInfoTable, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_text_color(_filesInfoTable, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(_filesInfoTable, color565(BACKGROUND_COL), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(_filesInfoTable, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(_filesInfoTable, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(_filesInfoTable, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(_filesInfoTable, 2, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(_filesInfoTable, 2, LV_PART_ITEMS);
    // Grid rules around every cell. v9 has no outline_side setter (the generated
    // per-side setters are gone), so this outlines all four sides - which is
    // what a table wants anyway.
    lv_obj_set_style_outline_width(_filesInfoTable, 1, LV_PART_ITEMS);
    lv_obj_set_style_outline_color(_filesInfoTable, color565(TFT_DARKGREY), LV_PART_ITEMS);
    lv_obj_set_style_outline_pad(_filesInfoTable, -1, LV_PART_ITEMS);
    for (int r = 0; r < kFileInfoRows; ++r)
    {
        lv_obj_t *fieldCell = lv_obj_get_child(_filesInfoTable, r * 2);
        if (fieldCell)
        {
            // Dim field names, bright values: the eye lands on the right half.
            lv_obj_set_style_text_color(fieldCell, lv_color_hex(0x8B97A8), 0);
        }
    }
    // Also attached to the TABLE itself, not just the container. LVGL bubbles an
    // unhandled press up through the parents, but a scrollable object consumes
    // presses for its own drag handling, so relying on bubbling alone would mean
    // a tap ON the table - the largest part of the screen - did nothing.
    lv_obj_add_event_cb(_filesInfoTable, filesPanelTouchEvent, LV_EVENT_PRESSED, nullptr);

    // Hidden until a row is selected and File info is tapped.
    lv_obj_add_flag(_filesInfoTable, LV_OBJ_FLAG_HIDDEN);

    // First-load indicator. It sits in the middle of the empty list area so the
    // screen says "working" even before the response arrives, instead of looking
    // like an empty list with a small "Loading..." line. Created last so it is
    // above the list and the info table; rotation is advanced by filesUpdate().
    _filesSpinner = lv_arc_create(parent);
    lv_obj_set_size(_filesSpinner, 48, 48);
    lv_obj_set_pos(_filesSpinner, (SCREEN_WIDTH - 48) / 2, 56 + (146 - 48) / 2);
    lv_arc_set_bg_angles(_filesSpinner, 0, 360);
    lv_arc_set_angles(_filesSpinner, 0, 90);
    lv_arc_set_rotation(_filesSpinner, 270);
    lv_obj_set_style_arc_color(_filesSpinner, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_color(_filesSpinner, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(_filesSpinner, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(_filesSpinner, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(_filesSpinner, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_remove_flag(_filesSpinner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(_filesSpinner, LV_OBJ_FLAG_HIDDEN);

    refreshFilesDetail();
}

void Display::filesPanelTouched()
{
    // Only meaningful while the details panel is up. LVGL bubbles events from the
    // touched object up through its parents, so this handler also sees taps on
    // the list and its rows - without this guard, choosing a file would bounce
    // straight back out of the list.
    if (_view != View::FileInfo)
        return;
    showFilesView();
}

void Display::showFilesView()
{
    if (_calActive)
    {
        showCalibrationView();
        return;
    }
    if (!_mainContainer || !_filesContainer)
        return;
    _view = View::Files;
    refreshFilesDetail();
    hideAllViews();
    lv_obj_clear_flag(_filesContainer, LV_OBJ_FLAG_HIDDEN);
    // Coming back from the details panel: put the list and its buttons back and
    // hide the panel. Done here rather than in showFileInfo() so there is exactly
    // one place that decides what the list view looks like.
    if (_filesInfoTable)
        lv_obj_add_flag(_filesInfoTable, LV_OBJ_FLAG_HIDDEN);
    if (_filesList)
        lv_obj_clear_flag(_filesList, LV_OBJ_FLAG_HIDDEN);
    if (_filesStatusLabel)
        lv_obj_clear_flag(_filesStatusLabel, LV_OBJ_FLAG_HIDDEN);
    if (_filesPrintButton)
        lv_obj_clear_flag(_filesPrintButton, LV_OBJ_FLAG_HIDDEN);
    if (_filesInfoButton)
        lv_obj_clear_flag(_filesInfoButton, LV_OBJ_FLAG_HIDDEN);
    // The list's own back button shares the top-left corner the panel's does.
    for (int i = 0; i < 2; ++i)
    {
        lv_obj_t *b = lv_obj_get_child(_filesContainer, i);
        if (b)
            lv_obj_clear_flag(b, LV_OBJ_FLAG_HIDDEN);
    }
}

void Display::showFileInfo()
{
    if (_calActive)
    {
        showCalibrationView();
        return;
    }
    if (!_filesInfoTable || _selectedFile < 0 || _selectedFile >= _filesCount)
        return;

    PrintFile entry;
    if (!pollerCopyFile(_selectedFile, entry))
        return;

    _fileInfoPath = entry.path;
    PrintFile info;
    uint32_t infoVersion = 0;
    bool infoBusy = false;
    bool infoReady = false;
    bool infoError = false;
    const bool haveSnapshot = pollerCopyFileInfo(info, infoVersion, infoBusy,
                                                 infoReady, infoError);
    const bool sameReady = haveSnapshot && infoReady &&
                           strcmp(info.path, _fileInfoPath.c_str()) == 0;
    _fileInfoVersionSeen = infoVersion;
    if (!sameReady)
        pollerRequestFileInfo(entry.path);

    // Swap the list out for the panel first, then paint. The detail fields show
    // "Loading..." until the poll task's response arrives.
    _view = View::FileInfo;
    if (_filesList)
        lv_obj_add_flag(_filesList, LV_OBJ_FLAG_HIDDEN);
    if (_filesStatusLabel)
        lv_obj_add_flag(_filesStatusLabel, LV_OBJ_FLAG_HIDDEN);
    if (_filesSpinner)
        lv_obj_add_flag(_filesSpinner, LV_OBJ_FLAG_HIDDEN);
    if (_filesPrintButton)
        lv_obj_add_flag(_filesPrintButton, LV_OBJ_FLAG_HIDDEN);
    if (_filesInfoButton)
        lv_obj_add_flag(_filesInfoButton, LV_OBJ_FLAG_HIDDEN);
    // Hide the list's own back button (child 0) now that the panel has its own.
    lv_obj_t *listBack = lv_obj_get_child(_filesContainer, 0);
    if (listBack)
        lv_obj_add_flag(listBack, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_filesInfoTable, LV_OBJ_FLAG_HIDDEN);

    refreshFileInfoDetail();
    Serial.printf("[FILES] info shown for index=%d path=%s\n", _selectedFile,
                  entry.path);
}

void Display::refreshFileInfoDetail()
{
    if (!_filesInfoTable || _fileInfoPath.length() == 0)
        return;

    PrintFile entry;
    if (!pollerCopyFileByPath(_fileInfoPath.c_str(), entry))
        return;

    PrintFile info;
    uint32_t infoVersion = 0;
    bool infoBusy = false;
    bool infoReady = false;
    bool infoError = false;
    const bool haveSnapshot =
        pollerCopyFileInfo(info, infoVersion, infoBusy, infoReady, infoError);
    const bool haveDetails = haveSnapshot && infoReady &&
                             strcmp(info.path, entry.path) == 0;
    const bool loading = !haveDetails && infoBusy;
    const bool failed = !haveDetails && !infoBusy && infoError;
    const char *missing = failed ? "unavailable" : "unknown";
    (void)infoVersion;

    char buf[64];
    String value;

    // Layer height and nozzle are GUESSED from the file name - OctoPrint exposes
    // neither, anywhere. They are already in the list entry, so they do not wait
    // for the detail request; the label says where they came from.
    lv_table_set_cell_value(_filesInfoTable, 0, 0, "File name");
    lv_table_set_cell_value(_filesInfoTable, 0, 1, entry.name);

    if (entry.layerHeightMm > 0.001f)
    {
        snprintf(buf, sizeof(buf), "%.2f mm  (from name)", entry.layerHeightMm);
        value = buf;
    }
    else
    {
        value = "unknown";
    }
    lv_table_set_cell_value(_filesInfoTable, 1, 0, "Layer height");
    lv_table_set_cell_value(_filesInfoTable, 1, 1, value.c_str());

    if (entry.nozzleMm > 0.001f)
    {
        snprintf(buf, sizeof(buf), "%.2f mm  (from name)", entry.nozzleMm);
        value = buf;
    }
    else
    {
        value = "unknown";
    }
    lv_table_set_cell_value(_filesInfoTable, 2, 0, "Nozzle");
    lv_table_set_cell_value(_filesInfoTable, 2, 1, value.c_str());

    if (haveDetails && info.material[0])
        value = info.material;
    else if (loading)
        value = "Loading...";
    else
        value = missing;
    lv_table_set_cell_value(_filesInfoTable, 3, 0, "Material");
    lv_table_set_cell_value(_filesInfoTable, 3, 1, value.c_str());

    if (haveDetails && info.estimatedPrintTime > 0)
    {
        value = formatDuration(info.estimatedPrintTime);
        if (info.averagePrintTime >= 1)
            value += String("  (avg ") + formatDuration(info.averagePrintTime) + ")";
    }
    else if (loading)
        value = "Loading...";
    else
        value = missing;
    lv_table_set_cell_value(_filesInfoTable, 4, 0, "Print time");
    lv_table_set_cell_value(_filesInfoTable, 4, 1, value.c_str());

    if (haveDetails && info.filamentLengthMm > 0)
    {
        snprintf(buf, sizeof(buf), "%.0f mm  (%.2f cm3)",
                 info.filamentLengthMm, info.filamentVolumeCm3);
        value = buf;
    }
    else if (loading)
        value = "Loading...";
    else
        value = missing;
    lv_table_set_cell_value(_filesInfoTable, 5, 0, "Filament");
    lv_table_set_cell_value(_filesInfoTable, 5, 1, value.c_str());

    if (haveDetails && info.dimX > 0 && info.dimY > 0 && info.dimZ > 0)
    {
        snprintf(buf, sizeof(buf), "%.0f x %.0f x %.0f mm", info.dimX, info.dimY, info.dimZ);
        value = buf;
    }
    else if (loading)
        value = "Loading...";
    else
        value = missing;
    lv_table_set_cell_value(_filesInfoTable, 6, 0, "Model size");
    lv_table_set_cell_value(_filesInfoTable, 6, 1, value.c_str());

    // These two are in the list entry, so they never wait on the request.
    lv_table_set_cell_value(_filesInfoTable, 7, 0, "File size");
    lv_table_set_cell_value(_filesInfoTable, 7, 1, formatBytes(entry.size).c_str());

    lv_table_set_cell_value(_filesInfoTable, 8, 0, "Uploaded");
    lv_table_set_cell_value(_filesInfoTable, 8, 1, formatUploadDate(entry.date).c_str());

    if (haveDetails)
    {
        if (info.printsSuccess == 0 && info.printsFailure == 0)
        {
            value = "never";
        }
        else
        {
            snprintf(buf, sizeof(buf), "%u ok", (unsigned)info.printsSuccess);
            value = buf;
            if (info.printsFailure)
            {
                value += ", ";
                snprintf(buf, sizeof(buf), "%u failed", (unsigned)info.printsFailure);
                value += buf;
            }
        }
    }
    else if (loading)
        value = "Loading...";
    else
        value = missing;
    lv_table_set_cell_value(_filesInfoTable, 9, 0, "Printed");
    lv_table_set_cell_value(_filesInfoTable, 9, 1, value.c_str());
}

void Display::openFilesView()
{
    if (_calActive)
    {
        showCalibrationView();
        return;
    }
    // Starting a print mid-job from a second client is rejected by the poll
    // task too; this keeps the entry itself idle-only, matching the panel
    // that carries the "No active print" text.
    if (_jobActive)
        return;
    _selectedFile = -1;
    // Show the cache immediately when it is still fresh. Otherwise fetch, but
    // keep the old rows visible if there are any; only an empty cache shows
    // "Loading...". The five-minute timer in filesUpdate() refreshes in place
    // while the view stays open.
    if (_filesOpenAction &&
        pollerFilesShouldRefresh(FILES_CACHE_TTL_MS, FILES_RETRY_MS))
        _filesOpenAction();
    showFilesView();
}

void Display::setFilesActions(Action open, FilePrintAction print)
{
    _filesOpenAction = open;
    _filePrintAction = print;
}

void Display::filesUpdate()
{
    if (!_filesList)
        return;
    uint8_t count = 0;
    uint32_t version = 0;
    bool busy = false;
    bool error = false;
    if (!pollerFilesMeta(count, version, busy, error))
        return;
    const bool versionMoved = version != _filesVersionSeen;
    const bool flagsMoved = busy != _filesBusy || error != _filesError ||
                            count != _filesCount;
    _filesCount = count;
    _filesVersionSeen = version;
    _filesBusy = busy;
    _filesError = error;
    if (versionMoved)
    {
        // The rows the selection pointed at may be gone after a refresh.
        _selectedFile = -1;
    }
    if (_view == View::Files && (versionMoved || flagsMoved))
    {
        refreshFilesDetail();
        if (versionMoved)
            lv_obj_scroll_to_y(_filesList, 0, LV_ANIM_OFF);
    }

    // The detail panel is a snapshot of one entry, so repaint only when the
    // poller says a detail request finished. The version does not move while
    // the list refreshes in the background because the panel is not open then.
    if (_view == View::FileInfo)
    {
        PrintFile info;
        uint32_t infoVersion = 0;
        bool infoBusy = false;
        bool infoReady = false;
        bool infoError = false;
        if (pollerCopyFileInfo(info, infoVersion, infoBusy, infoReady, infoError) &&
            infoVersion != _fileInfoVersionSeen)
        {
            _fileInfoVersionSeen = infoVersion;
            refreshFileInfoDetail();
        }
    }

    // Advance the first-load wheel only while it is on screen. filesUpdate()
    // runs at DISPLAY_REFRESH_TIME (250 ms), so 120 ms means one step per tick
    // with a little slack rather than a rotation that depends on loop speed.
    if (_filesSpinner && _view == View::Files && _filesBusy && _filesCount == 0)
    {
        const uint32_t now = millis();
        if ((uint32_t)(now - _filesSpinnerTick) >= 120)
        {
            _filesSpinnerTick = now;
            _filesSpinnerFrame = (_filesSpinnerFrame + 1) % 12;
            lv_arc_set_rotation(_filesSpinner, 270 + _filesSpinnerFrame * 30);
        }
    }

    // Five-minute check, only while the user is looking at the list and the
    // printer is idle. pollerFilesShouldRefresh() also enforces the TTL and
    // suppresses overlapping fetches, so this is a cheap test on every tick.
    if (_view == View::Files && !_jobActive && _filesOpenAction &&
        pollerFilesShouldRefresh(FILES_CACHE_TTL_MS, FILES_RETRY_MS))
        _filesOpenAction();
}

void Display::refreshFilesDetail()
{
    if (!_filesList || !_filesStatusLabel || !_filesPrintButton)
        return;
    if (_filesBusy && _filesCount == 0)
        setLabelTextOnce(_filesStatusLabel, "Loading...");
    else if (_filesError && _filesCount == 0)
        setLabelTextOnce(_filesStatusLabel, "Couldn't load files");
    else if (_filesCount == 0)
        setLabelTextOnce(_filesStatusLabel, "No files on OctoPrint");
    else if (_filesError)
        setLabelTextOnce(_filesStatusLabel, "Couldn't refresh");
    else
        setLabelTextOnce(_filesStatusLabel,
                          String(_filesCount) + (_filesCount == 1 ? " file" : " files") +
                              " - newest first");

    // Only a first load (nothing cached yet) gets the wheel. A background
    // five-minute refresh keeps its rows on screen, so an animation there would
    // say "empty" while the list is still usable.
    if (_filesSpinner)
    {
        if (_view == View::Files && _filesBusy && _filesCount == 0)
        {
            lv_obj_clear_flag(_filesSpinner, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(_filesSpinner, LV_OBJ_FLAG_HIDDEN);
            _filesSpinnerFrame = 0;
            _filesSpinnerTick = 0;
            lv_arc_set_rotation(_filesSpinner, 270);
        }
    }

    PrintFile entry;
    for (int index = 0; index < MAX_PRINT_FILES; ++index)
    {
        lv_obj_t *row = _fileRowLabels[index];
        if (!row)
            continue;
        if (index >= _filesCount || !pollerCopyFile(index, entry))
        {
            // A failed per-row copy keeps the previous text when the row is
            // still in range; only out-of-range rows are hidden.
            if (index >= _filesCount)
                lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
        // Selected like every other selection in this UI (material presets,
        // confirm Yes): dark green. Rows are opaque (bg_opa COVER at
        // creation), so the color actually renders. Only the selected row
        // rolls its text; the rest truncate (one rolling label at a time).
        const bool isSelected = index == _selectedFile;
        styleButton(row, isSelected ? TFT_DARKGREEN : TFT_DARKGREY);
        lv_label_set_long_mode(_fileRowLabels[index], isSelected
                                                            ? LV_LABEL_LONG_SCROLL_CIRCULAR
                                                            : LV_LABEL_LONG_DOT);
        setLabelTextOnce(_fileRowLabels[index], fileRowText(entry));
    }

    // The button stays visible with no selection, greyed out and inert;
    // selecting a row enables it. The confirm dialog re-validates anyway.
    const bool hasSelection = _selectedFile >= 0 && _selectedFile < _filesCount;
    if (hasSelection)
    {
        styleButton(_filesPrintButton, TFT_DARKGREEN);
        lv_obj_set_style_bg_opa(_filesPrintButton, LV_OPA_COVER, 0);
        lv_obj_set_style_text_opa(_filesPrintButton, LV_OPA_COVER, 0);
        lv_obj_add_flag(_filesPrintButton, LV_OBJ_FLAG_CLICKABLE);
    }
    else
    {
        styleButton(_filesPrintButton, TFT_DARKGREY);
        lv_obj_set_style_bg_opa(_filesPrintButton, LV_OPA_50, 0);
        lv_obj_set_style_text_opa(_filesPrintButton, LV_OPA_50, 0);
        lv_obj_clear_flag(_filesPrintButton, LV_OBJ_FLAG_CLICKABLE);
    }

    // Same rule for File info: both act on the selected row, so one greys out
    // with the other. Written as its own block rather than a loop because the
    // two have different colours.
    if (_filesInfoButton)
    {
        if (hasSelection)
        {
            styleButton(_filesInfoButton, TFT_BLUE);
            lv_obj_set_style_bg_opa(_filesInfoButton, LV_OPA_COVER, 0);
            lv_obj_set_style_text_opa(_filesInfoButton, LV_OPA_COVER, 0);
            lv_obj_add_flag(_filesInfoButton, LV_OBJ_FLAG_CLICKABLE);
        }
        else
        {
            styleButton(_filesInfoButton, TFT_DARKGREY);
            lv_obj_set_style_bg_opa(_filesInfoButton, LV_OPA_50, 0);
            lv_obj_set_style_text_opa(_filesInfoButton, LV_OPA_50, 0);
            lv_obj_clear_flag(_filesInfoButton, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

void Display::selectFile(int index)
{
    if (index < 0 || index >= _filesCount)
        index = -1;
    if (index == _selectedFile)
        return;
    _selectedFile = index;
    Serial.printf("[FILES] select %d\n", index);
    refreshFilesDetail();
}

void Display::showPrintConfirm()
{
    if (!_confirmOverlay || _selectedFile < 0 || _selectedFile >= _filesCount)
        return;
    PrintFile entry;
    if (!pollerCopyFile(_selectedFile, entry))
        return;
    _pendingJob = PRINT_FILE;
    setLabelTextOnce(_confirmLabel, "Print file?");
    setLabelTextOnce(_confirmSubLabel, entry.name);
    styleButton(_confirmYesButton, TFT_DARKGREEN);
    // Same single-fire discipline as Pause/Stop: retarget on top, one Yes ==
    // one staged print even on rapid double-taps.
    lv_obj_move_to_index(_confirmOverlay, -1);
    lv_obj_clear_flag(_confirmOverlay, LV_OBJ_FLAG_HIDDEN);
    Serial.printf("[FILES] print confirm shown %s\n", entry.path);
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
    cmd.calAction = _calAction;
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
    calibrationUpdate();
    refreshChamberDetail();
    refreshEnclosureLinkHeader();
}

void Display::calibrationUpdate()
{
    if (!_calContainer)
        return;

    const unsigned long now = millis();
    const bool fresh = _enclosure.valid &&
                       (millis() - _enclosure.rxMs < ENC_LINK_STALE_MS);

    if (fresh)
    {
        _calPhase = _enclosure.calPhase;
        _calLinkLostMs = 0;
        const bool phaseActive = calPhaseActive(_calPhase);

        // Rising edge: latch the modal before doing anything else, so a run
        // that starts while the user is in the file list cannot leave that
        // list on screen with the relay active underneath.
        if (phaseActive && !_calActive)
        {
            _calActive = true;
            _calCancelSent = false;
            _calEndedMs = 0;
            // Re-show the modal if it is not the visible view. `_calShown` can
            // still be true while something else is on screen (a failed OTA
            // restored the main view, or a job transition replaced the final
            // result screen), and a new run must latch the lock either way.
            // Never paint over an active OTA splash; hideSplash() restores the
            // modal once the upload has finished or failed.
            if ((!_calShown || _view != View::Calibration) && !_splashSpinner)
                showCalibrationView();
        }
        // Falling edge: keep the modal up for the result, but release
        // navigation. This only runs on FRESH telemetry - a stale snapshot
        // must not be mistaken for "the run ended", or a five-second Wi-Fi
        // hiccup would silently drop the modal lock.
        else if (!phaseActive && _calActive)
        {
            _calActive = false;
            _calCancelSent = false;
            _calEndedMs = now;
            // The C3 has processed (or never needed) the one-shot. Returning
            // the field to zero lets the next run start from a defined value;
            // the C3 uses that zero to re-arm its remembered action. Always
            // send it, not just when this side still thinks it is nonzero: the
            // link task repeats lastCmd between UI changes, and a reset that
            // was queued but never drained would otherwise leave the heartbeat
            // carrying a stale cancel id.
            _calAction = 0;
            pushEnclosureCommand();
            if (!_calShown && !_splashSpinner)
                showCalibrationView();
        }
    }
    else if (_calActive)
    {
        // No fresh telemetry. Keep the run latched for a short grace period,
        // then release the screen: the C3 is unreachable, so cancelling from
        // here is impossible and a powered-off board must not leave the
        // touchscreen permanently unusable. If telemetry returns with an
        // active phase, the rising edge above re-latches immediately.
        if (_calLinkLostMs == 0)
            _calLinkLostMs = now;
        if (now - _calLinkLostMs >= CAL_LINK_LOST_UNLOCK_MS)
        {
            Serial.println("[CAL] C3 link lost; releasing modal screen");
            _calActive = false;
            _calCancelSent = false;
            // Unconditionally re-send the zero for the same reason as the
            // normal falling edge above: lastCmd may still hold an older id.
            _calAction = 0;
            pushEnclosureCommand();
            _calEndedMs = now;
        }
    }

    if (_calShown)
        refreshCalibrationDetail();

    // Auto-close only the result screen. While _calActive is true this cannot
    // fire, which is what makes the screen modal for the whole run.
    if (_calShown && !_calActive && _calEndedMs != 0 &&
        now - _calEndedMs >= CAL_RESULT_HOLD_MS)
    {
        closeCalibrationView();
    }
}

void Display::refreshCalibrationDetail()
{
    if (!_calBodyLabel || !_calButton || !_calButtonLabel)
        return;

    const bool fresh = _enclosure.valid &&
                       (millis() - _enclosure.rxMs < ENC_LINK_STALE_MS);
    char chamber[18];
    char bed[18];
    char elapsed[18];
    char body[384];
    formatCx10(_enclosure.chamberCx10, chamber, sizeof(chamber));
    formatCx10(_enclosure.bedCx10, bed, sizeof(bed));
    formatCalElapsed(_enclosure.calElapsedSec, elapsed, sizeof(elapsed));

    if (_calActive)
    {
        if (!fresh)
        {
            snprintf(body, sizeof(body),
                     "C3 link lost\n\n"
                     "Calibration state is unknown.\n"
                     "The screen will unlock after %lu s if the link does not return.",
                     (unsigned long)(CAL_LINK_LOST_UNLOCK_MS / 1000UL));
        }
        else
        {
            snprintf(body, sizeof(body),
                     "Phase: %s\n"
                     "Chamber: %s / %u C\n"
                     "Bed: %s / %u C\n"
                     "Elapsed: %s\n"
                     "Cycles: %u\n\n"
                     "Navigation is locked until the run ends.",
                     calPhaseText(_calPhase), chamber,
                     (unsigned)_enclosure.calSetpointC, bed,
                     (unsigned)_enclosure.calBedTargetC, elapsed,
                     (unsigned)_enclosure.calCycles);
        }
    }
    else if (_calLinkLostMs != 0)
    {
        snprintf(body, sizeof(body),
                 "C3 link lost\n\n"
                 "Calibration state is unknown.\n"
                 "Check the C3; this screen is no longer locked.");
    }
    else if (_calPhase == ENC_CAL_PHASE_DONE)
    {
        snprintf(body, sizeof(body),
                 "Phase: %s\n"
                 "Chamber: %s / %u C\n"
                 "Bed: %s / %u C\n"
                 "Elapsed: %s\n"
                 "Cycles: %u\n\n"
                 "Result ready. Apply or discard it in the C3 web UI.",
                 calPhaseText(_calPhase), chamber,
                 (unsigned)_enclosure.calSetpointC, bed,
                 (unsigned)_enclosure.calBedTargetC, elapsed,
                 (unsigned)_enclosure.calCycles);
    }
    else if (_calPhase == ENC_CAL_PHASE_ABORTED)
    {
        snprintf(body, sizeof(body),
                 "Phase: %s\n"
                 "Reason: %s\n"
                 "Chamber: %s / %u C\n"
                 "Bed: %s / %u C\n"
                 "Elapsed: %s\n\n"
                 "The bed was switched off.",
                 calPhaseText(_calPhase),
                 calErrorText(_enclosure.calErrorCode), chamber,
                 (unsigned)_enclosure.calSetpointC, bed,
                 (unsigned)_enclosure.calBedTargetC, elapsed);
    }
    else
    {
        snprintf(body, sizeof(body), "Calibration is not running.");
    }

    const char *current = lv_label_get_text(_calBodyLabel);
    if (!current || strcmp(current, body) != 0)
        lv_label_set_text(_calBodyLabel, body);

    if (_calActive)
    {
        setLabelTextOnce(_calButtonLabel, _calCancelSent ? "Cancelling..." : "Cancel calibration");
        styleButton(_calButton, _calCancelSent ? TFT_DARKGREY : TFT_RED);
    }
    else
    {
        setLabelTextOnce(_calButtonLabel, "Close");
        styleButton(_calButton, TFT_DARKGREEN);
    }
}

void Display::calibrationButtonTapped()
{
    if (!_calActive)
    {
        closeCalibrationView();
        return;
    }
    if (_calCancelSent)
        return;
    showCalibrationConfirm();
}

void Display::closeCalibrationView()
{
    _calShown = false;
    _calCancelSent = false;
    // Unconditionally re-send zero: this also repairs lastCmd if an earlier
    // reset was queued but dropped, and it resumes the C3's normal command
    // stream with a known one-shot state.
    _calAction = 0;
    pushEnclosureCommand();
    _calEndedMs = 0;
    _calLinkLostMs = 0;
    if (_view == View::Calibration)
        showMainView();
}

void Display::showCalibrationConfirm()
{
    if (!_confirmOverlay || !_calActive)
        return;
    _pendingJob = CAL_CANCEL;
    setLabelTextOnce(_confirmLabel, "Cancel calibration?");
    setLabelTextOnce(_confirmSubLabel,
                     "The bed will be switched off and this run discarded. "
                     "This cannot be undone.");
    styleButton(_confirmYesButton, TFT_RED);
    lv_obj_move_to_index(_confirmOverlay, -1);
    lv_obj_clear_flag(_confirmOverlay, LV_OBJ_FLAG_HIDDEN);
    Serial.println("[CAL] cancel confirm shown");
}

void Display::refreshEnclosureLinkHeader()
{
    const bool fresh = _enclosure.valid && (millis() - _enclosure.rxMs < ENC_LINK_STALE_MS);

    // C3 icon tracks telemetry freshness, whatever the transport.
    if (_encLinkIcon && fresh != _encLinkUp)
    {
        _encLinkUp = fresh;
        lv_obj_set_style_image_recolor(_encLinkIcon, color565(fresh ? TFT_GREEN : 0x808080), 0);
    }

    // API icon appears only while the HTTP fallback is the live carrier (fresh
    // AND the newest telemetry came from the API). Hidden otherwise, so its
    // presence itself reports that the API connection is established.
    if (_apiLinkIcon)
    {
        const bool apiUp = fresh && _enclosure.fromApi;
        if (apiUp != _apiLinkUp)
        {
            _apiLinkUp = apiUp;
            if (apiUp)
                lv_obj_clear_flag(_apiLinkIcon, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(_apiLinkIcon, LV_OBJ_FLAG_HIDDEN);
        }
    }
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

void Display::setButtonActions(Action pause, Action stop, Action resume)
{
    buttonActions[0] = pause;
    buttonActions[1] = stop;
    buttonActions[2] = resume;
}

void Display::createJobConfirm()
{
    // Built once and reused (show/hide) so Pause/Stop taps never malloc in
    // the steady state - same heap discipline as the rest of this file.
    // Child of the screen (not _mainContainer) so a view switch cannot bury
    // it; created after the view containers so it starts on top.
    _pendingJob = -1;
    _confirmOverlay = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(_confirmOverlay, 0, 0);
    lv_obj_set_size(_confirmOverlay, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(_confirmOverlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_confirmOverlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(_confirmOverlay, 0, 0);
    lv_obj_set_style_radius(_confirmOverlay, 0, 0);
    lv_obj_set_style_pad_all(_confirmOverlay, 0, 0);
    lv_obj_clear_flag(_confirmOverlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(_confirmOverlay, LV_SCROLLBAR_MODE_OFF);
    // Plain objects are not clickable by default - without this, taps on the
    // dim background never produce LV_EVENT_CLICKED and outside-tap-to-cancel
    // would silently not work.
    lv_obj_add_flag(_confirmOverlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_confirmOverlay, confirmOverlayEvent, LV_EVENT_CLICKED, nullptr);

    _confirmBox = lv_obj_create(_confirmOverlay);
    lv_obj_set_size(_confirmBox, 260, 140);
    lv_obj_align(_confirmBox, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_confirmBox, color565(BACKGROUND_COL), 0);
    lv_obj_set_style_border_color(_confirmBox, lv_color_hex(0x5A5A5A), 0);
    lv_obj_set_style_border_width(_confirmBox, 1, 0);
    lv_obj_set_style_radius(_confirmBox, 3, 0);
    lv_obj_set_style_pad_all(_confirmBox, 8, 0);
    lv_obj_clear_flag(_confirmBox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(_confirmBox, LV_SCROLLBAR_MODE_OFF);

    _confirmLabel = lv_label_create(_confirmBox);
    lv_label_set_text(_confirmLabel, "Pause print?");
    lv_obj_set_style_text_color(_confirmLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(_confirmLabel, &lv_font_montserrat_20, 0);
    lv_obj_align(_confirmLabel, LV_ALIGN_TOP_MID, 0, 4);

    _confirmSubLabel = lv_label_create(_confirmBox);
    lv_label_set_text(_confirmSubLabel, "");
    styleLabel(_confirmSubLabel);
    lv_obj_set_width(_confirmSubLabel, 230);
    lv_label_set_long_mode(_confirmSubLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(_confirmSubLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_confirmSubLabel, LV_ALIGN_TOP_MID, 0, 32);

    _confirmYesButton = lv_button_create(_confirmBox);
    lv_obj_set_size(_confirmYesButton, 110, 40);
    lv_obj_align(_confirmYesButton, LV_ALIGN_BOTTOM_LEFT, 4, -4);
    styleButton(_confirmYesButton, TFT_DARKGREEN);
    lv_obj_add_event_cb(_confirmYesButton, confirmYesEvent, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *yesLabel = lv_label_create(_confirmYesButton);
    lv_label_set_text(yesLabel, "Yes");
    lv_obj_set_style_text_color(yesLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_remove_flag(yesLabel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(yesLabel);

    _confirmCancelButton = lv_button_create(_confirmBox);
    lv_obj_set_size(_confirmCancelButton, 110, 40);
    lv_obj_align(_confirmCancelButton, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
    styleButton(_confirmCancelButton, TFT_DARKGREY);
    lv_obj_add_event_cb(_confirmCancelButton, confirmCancelEvent, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cancelLabel = lv_label_create(_confirmCancelButton);
    lv_label_set_text(cancelLabel, "Cancel");
    lv_obj_set_style_text_color(cancelLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_remove_flag(cancelLabel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(cancelLabel);

    lv_obj_add_flag(_confirmOverlay, LV_OBJ_FLAG_HIDDEN);
}

void Display::showJobConfirm(int jobCommand)
{
    if (!_confirmOverlay)
        return;
    if (jobCommand != PAUSE_JOB && jobCommand != STOP_JOB)
        return;
    _pendingJob = jobCommand;
    const bool isPause = jobCommand == PAUSE_JOB;
    setLabelTextOnce(_confirmLabel, isPause ? "Pause print?" : "Cancel print?");
    setLabelTextOnce(_confirmSubLabel, isPause ? "The print will pause."
                                               : "The job will be cancelled. This cannot be undone.");
    styleButton(_confirmYesButton, isPause ? TFT_ORANGE : TFT_RED);
    // Retargeting the same overlay keeps one Yes tap == one command even on
    // rapid double-taps: the second Pause/Stop press just refreshes the text.
    lv_obj_move_to_index(_confirmOverlay, -1);
    lv_obj_clear_flag(_confirmOverlay, LV_OBJ_FLAG_HIDDEN);
    Serial.printf("[JOB] confirm shown cmd=%d\n", jobCommand);
}

void Display::hideJobConfirm()
{
    if (!_confirmOverlay)
    {
        _pendingJob = -1;
        return;
    }
    if (_pendingJob < 0 && lv_obj_has_flag(_confirmOverlay, LV_OBJ_FLAG_HIDDEN))
        return;
    _pendingJob = -1;
    if (_confirmOverlay)
        lv_obj_add_flag(_confirmOverlay, LV_OBJ_FLAG_HIDDEN);
    Serial.printf("[JOB] confirm dismissed\n");
}

void Display::confirmPendingJob()
{
    const int pending = _pendingJob;
    if (pending == CAL_CANCEL)
    {
        _pendingJob = -1;
        if (_confirmOverlay)
            lv_obj_add_flag(_confirmOverlay, LV_OBJ_FLAG_HIDDEN);
        if (!_calActive || _calCancelSent)
            return;
        // Advance the action id so the C3 sees a CHANGE exactly once. The
        // heartbeat repeats this value; the C3 ignores repeats, so a later run
        // cannot be cancelled by this stale command.
        _calAction = (_calAction == 255) ? 1 : (uint8_t)(_calAction + 1);
        _calCancelSent = true;
        Serial.printf("[CAL] cancel requested action=%u\n", (unsigned)_calAction);
        pushEnclosureCommand();
        refreshCalibrationDetail();
        return;
    }
    if (pending == PRINT_FILE)
    {
        _pendingJob = -1;
        if (_confirmOverlay)
            lv_obj_add_flag(_confirmOverlay, LV_OBJ_FLAG_HIDDEN);
        // Re-fetch the path: the list may have refreshed between open and
        // Yes, and the poll task re-validates the selection anyway.
        PrintFile entry;
        if (_selectedFile >= 0 && _selectedFile < _filesCount &&
            pollerCopyFile(_selectedFile, entry) && _filePrintAction)
        {
            Serial.printf("[FILES] print confirm yes %s\n", entry.path);
            _filePrintAction(entry.path);
        }
        // Stay on the files view: setJobState() brings the main view back
        // once the job actually shows up as active.
        return;
    }
    if (pending != PAUSE_JOB && pending != STOP_JOB)
    {
        hideJobConfirm();
        return;
    }
    const size_t index = (pending == PAUSE_JOB) ? 0 : 1;
    _pendingJob = -1;
    if (_confirmOverlay)
        lv_obj_add_flag(_confirmOverlay, LV_OBJ_FLAG_HIDDEN);
    Serial.printf("[JOB] confirm yes cmd=%d\n", pending);
    if (buttonActions[index])
        buttonActions[index]();
}

void Display::setToolActions(Action pla, Action petg, Action asa, Action off)
{
    toolActions[0] = pla;
    toolActions[1] = petg;
    toolActions[2] = asa;
    toolActions[3] = off;
}

void Display::setPreheatAction(PreheatKind kind, PreheatAction action)
{
    _preheatAction[static_cast<size_t>(kind)] = action;
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
        // Print-button geometry (which two are visible, how wide) is owned by
        // refreshJobButtons(), not by the active/idle layout switch.
        for (int index = 0; index < 4; ++index)
            lv_obj_set_pos(_preheatButtons[index], preheatX[index], 180);
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
        setLabelTextOnce(_fileNameLabel, "Select file...");
        lv_obj_center(_fileNameLabel);
        for (int index = 0; index < 4; ++index)
            lv_obj_set_pos(_preheatButtons[index], preheatX[index], 190);
    }
}

void Display::setJobState(bool active, bool paused)
{
    // Paused is only meaningful with a job up; without one it would leave a
    // stale Resume visible after the job ends.
    const bool pausedNow = active && paused;
    const bool changed = !_jobLayoutInitialized || _jobLayoutActive != active || _jobPaused != pausedNow;
    _jobActive = active;
    _jobPaused = pausedNow;
    // Bed targets go inert mid-print regardless of which view is up, so this sits
    // outside the layout guard.
    setPreheatControlsEnabled(PreheatKind::Tool, !active);
    setPreheatControlsEnabled(PreheatKind::Bed, !active);
    if (changed)
    {
        _jobLayoutInitialized = true;
        _jobLayoutActive = active;
        if (!active)
        {
            // A Yes tapped after the job ended must not fire: drop any open
            // Pause/Cancel confirm together with the buttons themselves.
            hideJobConfirm();
            clearJobDisplay();
        }
        else if (pausedNow && _pendingJob == PAUSE_JOB)
        {
            // Paused from elsewhere (OctoPrint, another client) while our
            // Pause confirm was open: the Pause button just hid, so Yes would
            // be a stale fire. The Cancel confirm stays valid in both states.
            hideJobConfirm();
        }
        applyMainJobLayout(active);
        setPreheatButtonsVisibility(!active);
        refreshJobButtons();
        // A print started from the files view (or another client) takes over
        // the screen: the progress UI lives on the main view.
        if (active && _view == View::Files)
            showMainView();
    }
    if (changed || _enclosureCommandDirty)
    {
        _enclosureCommandDirty = false;
        pushEnclosureCommand();
    }
}

void Display::setPreheatButtonsVisibility(bool visible)
{
    setButtonVisibility(_preheatButtons, 4, visible);
}

void Display::refreshJobButtons()
{
    if (!_printButtons[0] || !_printButtons[1] || !_printButtons[2])
        return;
    // Pause matters only while printing, Resume only while paused, Cancel
    // whenever a job is up. Null-safe before createLabes() finishes.
    const bool printing = _jobActive && !_jobPaused;
    const bool paused = _jobActive && _jobPaused;
    lv_obj_set_hidden(_printButtons[0], !printing);
    lv_obj_set_hidden(_printButtons[1], !_jobActive);
    lv_obj_set_hidden(_printButtons[2], !paused);
    if (!_jobActive)
        return;
    // The two visible buttons fill the row: 5 + 150 + 5 + 155 + 5 = 320.
    // Pause and Resume share the left slot; Cancel owns the right one.
    lv_obj_t *left = paused ? _printButtons[2] : _printButtons[0];
    lv_obj_set_pos(left, 5, 180);
    lv_obj_set_size(left, 150, 40);
    lv_obj_set_pos(_printButtons[1], 160, 180);
    lv_obj_set_size(_printButtons[1], 155, 40);
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
    updateTempLabel(_toolLabel, snap.toolActual, snap.toolTarget);
    updateTempLabel(_bedLabel, snap.bedActual, snap.bedTarget);

    // The heat waves above the bed take the temperature color (cold blue,
    // warm orange, hot red); the bed body underneath stays its native
    // orange. Recolored only when the band changes - same heap discipline
    // as the labels above. The orange band runs to 100 so a
    // 100 C setpoint plus normal overshoot/ripple does not flicker red.
    if (_bedWavesIcon)
    {
        const uint8_t band = snap.bedActual < 50 ? 0 : (snap.bedActual <= 100 ? 1 : 2);
        if (band != _bedWavesBand)
        {
            _bedWavesBand = band;
            static const uint16_t bandColors[] = {TFT_BLUE, TFT_ORANGE, TFT_RED};
            lv_obj_set_style_image_recolor(_bedWavesIcon, color565(bandColors[band]), 0);
            Serial.printf("[BED] waves band=%u temp=%.1f\n", band, snap.bedActual);
        }
    }

    // Feed both preheat screens from the same snapshot.
    _preheat[static_cast<size_t>(PreheatKind::Tool)].actual = snap.toolActual;
    _preheat[static_cast<size_t>(PreheatKind::Tool)].actualValid = true;
    _preheat[static_cast<size_t>(PreheatKind::Bed)].actual = snap.bedActual;
    _preheat[static_cast<size_t>(PreheatKind::Bed)].actualValid = true;

    // Adopt OctoPrint's target as the truth rather than echoing what we last
    // sent, so a change made from a gcode macro or from OctoPrint itself shows up
    // here too. But the poll runs slower than a tap, so a snapshot that predates
    // the last press would drag the label backwards; hold our own value briefly
    // after sending.
    for (size_t slot = 0; slot < kPreheatCount; ++slot)
    {
        const auto kind = static_cast<PreheatKind>(slot);
        if (_view != preheatView(kind))
            continue;
        const int polled = kind == PreheatKind::Tool ? snap.toolTarget : snap.bedTarget;
        PreheatView &view = _preheat[slot];
        if (!_jobActive && millis() - view.sentMs > PREHEAT_TARGET_SETTLE_MS)
            view.target = constrain(polled, preheatMin(kind), preheatMax(kind));
        refreshPreheatDetail(kind);
    }

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
