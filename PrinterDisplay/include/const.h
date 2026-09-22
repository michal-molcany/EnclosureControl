#ifndef const_h
#define const_h

#define OCTOTPRINT_PORT 80            // Port of the OctoPrint server
#define OCTOTPRINT_IP "10.201.93.207" // IP address of the OctoPrint server (change this to your OctoPrint server IP address)

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

#define API_REFRESH_TIME 3000 // 3 seconds (background poll cycle)
#define DISPLAY_REFRESH_TIME 250 // 0.25 seconds (UI redraw from snapshot)

#define LABEL1_FONT &FreeSerif8pt7b      // Key label font 1
#define LABEL2_FONT &FreeSerifBold12pt7b // Key label font 2

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define FONT_SIZE 2
#define BACKGROUND_COL TFT_BLACK
#define BACKGROUND 0x18E3
#define FONT_COL TFT_WHITE

#define SERIAL_DEBUG 0

enum ButtonFunction
{
    PREHEAT_215 = 215,
    PREHEAT_230 = 230,
    PREHEAT_OFF = 0,
    PAUSE_PRINT_BUTTON = -1,
    RESUME_PRINT_BUTTON = -2,
    STOP_PRINT_BUTTON = -3,
};

#endif