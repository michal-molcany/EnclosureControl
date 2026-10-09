// LVGL configuration for the CYD.
//
// This file is included via LV_CONF_INCLUDE_SIMPLE, so every setting NOT defined
// here falls back to lvgl/config/lv_conf_internal.h. Without a Kconfig present
// that file defaults every widget and feature to 1, which means LVGL is free to
// compile in code this UI never touches. The CYD firmware sits at ~96% of its
// 1.25MB app partition, so those defaults are not affordable.
//
// Rule applied below: a feature is enabled only if the UI actually uses it.
// Verified against src/display.cpp, which constructs exactly six widgets -
// lv_label, lv_button, lv_obj, lv_tabview, lv_bar, lv_arc - plus lv_image for the
// icons in include/icons.h.

#ifndef LV_CONF_H
#define LV_CONF_H

/* ---- Rendering ---- */
#define LV_COLOR_DEPTH 16
#define LV_HOR_RES_MAX 320
#define LV_VER_RES_MAX 240

/* ---- Memory ---- */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (60U * 1024U)

/* ---- Widgets actually used ---- */
#define LV_USE_OBJ 1
#define LV_USE_LABEL 1
#define LV_USE_BUTTON 1
#define LV_USE_BAR 1
#define LV_USE_ARC 1
#define LV_USE_TABVIEW 1
#define LV_USE_IMAGE 1

/* ---- Layout engines in use ---- */
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/* ---- Theme: default only, so appearance is unchanged ---- */
#define LV_USE_THEME_DEFAULT 1

/* ---- Fonts ---- */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* ==========================================================================
   Everything below is unused by this UI.

   These default to 1 in lv_conf_internal.h, so leaving them out would compile
   them in. lv_calendar_constructor and lv_dropdown_class were both present in
   the 1.23MB binary of a UI that has no calendar and no dropdown.

   If a future change needs one of these, flip it back to 1 - that is the whole
   cost of re-enabling, and the build will tell you immediately if something
   internal still depends on it.
   ========================================================================== */

/* Unused widgets. */
#define LV_USE_ANIMIMG 0
#define LV_USE_ARCLABEL 0
#define LV_USE_BUTTONMATRIX 0
#define LV_USE_CALENDAR 0
#define LV_USE_CALENDAR_HEADER_ARROW 0
#define LV_USE_CALENDAR_HEADER_DROPDOWN 0
#define LV_USE_CANVAS 0
#define LV_USE_CHART 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LED 0
#define LV_USE_LINE 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_MSGBOX 0
#define LV_USE_ROLLER 0
#define LV_USE_SCALE 0
#define LV_USE_SLIDER 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_SWITCH 0
#define LV_USE_TABLE 0
#define LV_USE_TEXTAREA 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0

/* Themes other than the default one. */
#define LV_USE_THEME_MONO 0
#define LV_USE_THEME_SIMPLE 0

/* Font engines and text features. The two Montserrat fonts above are plain
   compiled-in bitmaps, not TrueType and not compressed, so none of these are
   needed. FreeType alone was costing 8.6KB. */
#define LV_USE_FREETYPE 0
#define LV_USE_TINY_TTF 0
#define LV_USE_FONT_MANAGER 0
#define LV_USE_FONT_PLACEHOLDER 0
#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_IMGFONT 0
#define LV_USE_BIDI 0
#define LV_USE_ARABIC_PERSIAN_CHARS 0
#define LV_USE_IME_PINYIN 0
#define LV_USE_COLOR_FILTER 0

/* Allocator: NOT disabled. LV_USE_STDLIB_MALLOC above already selects the
   builtin pool, so the TLSF copy is dead weight, but LVGL 9.5 cannot be built
   with it off - lv_global.h declares lv_tlsf_state_t unconditionally and every
   core object fails to compile. Setting LV_USE_TLSF 0 costs 4KB of flash. */

/* Input, debug and diagnostics. The touchscreen is driven by XPT2046 over
   SPI and read through lv_indev, not through any of LVGL's own input backends. */
#define LV_USE_EVDEV 0
#define LV_USE_GRIDNAV 0
#define LV_USE_GESTURE_RECOGNITION 0
#define LV_USE_INDEV 1

#define LV_USE_LOG 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_PERF_MONITOR_LOG_MODE 0
#define LV_USE_SNAPSHOT 0
// OFF: briefly enabled with LV_USE_SYSMON to read pool headroom via
// lv_mem_monitor() after the files-view pool-exhaustion crash, but the
// monitor draws its own overlay ("48.6kB (92%)" etc.) on the screen, which
// does not belong on the user-facing UI. Pool headroom is verified; the
// files rows stay single clickable labels (one LVGL object per row).
#define LV_USE_MEM_MONITOR 0
#define LV_USE_SYSMON 0
#define LV_USE_PROFILER 0
#define LV_USE_LAYER_DEBUG 0
#define LV_USE_REFR_DEBUG 0
#define LV_USE_ASSERT 0
#define LV_USE_ASSERT_MALLOC 0
#define LV_USE_ASSERT_NULL 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_CHECK_ARG 0
#define LV_USE_CHECK_OBJ_CLASSTYPE 0
#define LV_USE_CHECK_OBJ_PARENT_LINK 0
#define LV_USE_CHECK_OBJ_VALIDITY 0
#define LV_USE_TEST 0
#define LV_USE_MONKEY 0
#define LV_USE_FILE_EXPLORER 0

#endif // LV_CONF_H
