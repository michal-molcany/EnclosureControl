#ifndef const_h
#define const_h


/*Set to your screen resolution and rotation*/
#define TFT_HOR_RES 240
#define TFT_VER_RES 320
#define TFT_ROTATION LV_DISPLAY_ROTATION_90

// Touchscreen pins
#define XPT2046_IRQ 36  // T_IRQ
#define XPT2046_MOSI 32 // T_DIN
#define XPT2046_MISO 39 // T_OUT
#define XPT2046_CLK 25  // T_CLK
#define XPT2046_CS 33   // T_CS

#define API_REFRESH_TIME 1000 // 1 seconds

#define LABEL1_FONT &FreeSerif8pt7b      // Key label font 1
#define LABEL2_FONT &FreeSerifBold12pt7b // Key label font 2

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define FONT_SIZE 2
#define BACKGROUND_PANEL TFT_BLACK
#define BACKGROUND 0x18E3
#define FONT_COL TFT_WHITE

#define BUTTON_W 80
#define BUTTON_H 40

#define MAIN_SCREEN_BUTTONS 3

enum ButtonFunction
{
    PREHEAT_215 = 215,
    PREHEAT_230 = 230,
    PREHEAT_OFF = 0,
    PAUSE_PRINT_BUTTON = -1,
    RESUME_PRINT_BUTTON = -2,
    CANCEL_PRINT_BUTTON = -3,
};

#endif