#pragma once

/* Per-board geometry and fonts.

   Two boards are supported, picked by the build target:

     esp32c6  Seeed XIAO ESP32-C6 + Round Display
              GC9A01A 240x240 over SPI, CHSC6X touch
     esp32s3  Waveshare ESP32-S3-Touch-LCD-1.46
              SPD2010 412x412 over QSPI, SPD2010 touch

   The UI is authored against the 240px board and scaled up with UI_PX(), so
   layout stays defined in one place regardless of which panel it lands on. */

#include "lvgl.h"

#if CONFIG_IDF_TARGET_ESP32S3

#define BOARD_NAME  "Waveshare ESP32-S3-Touch-LCD-1.46"
#define UI_SIZE     412

LV_FONT_DECLARE(montserrat_ext_21);
LV_FONT_DECLARE(montserrat_ext_24);
LV_FONT_DECLARE(montserrat_ext_28);
LV_FONT_DECLARE(montserrat_ext_31);
LV_FONT_DECLARE(montserrat_ext_34);

#define FONT_CAPTION  (&montserrat_ext_21)   /* 12px at 240 */
#define FONT_SMALL    (&montserrat_ext_24)   /* 14px */
#define FONT_BODY     (&montserrat_ext_28)   /* 16px */
#define FONT_TITLE    (&montserrat_ext_31)   /* 18px */
#define FONT_LARGE    (&montserrat_ext_34)   /* 20px */

#else

#define BOARD_NAME  "Seeed XIAO Round Display"
#define UI_SIZE     240

LV_FONT_DECLARE(montserrat_ext_12);
LV_FONT_DECLARE(montserrat_ext_14);
LV_FONT_DECLARE(montserrat_ext_16);
LV_FONT_DECLARE(montserrat_ext_18);
LV_FONT_DECLARE(montserrat_ext_20);

#define FONT_CAPTION  (&montserrat_ext_12)
#define FONT_SMALL    (&montserrat_ext_14)
#define FONT_BODY     (&montserrat_ext_16)
#define FONT_TITLE    (&montserrat_ext_18)
#define FONT_LARGE    (&montserrat_ext_20)

#endif

/* Scale a length authored against the 240px reference screen, rounding to
   nearest so symmetric pairs (e.g. ±60) stay symmetric. */
static inline int ui_px(int v)
{
    return (v * UI_SIZE + (v >= 0 ? 120 : -120)) / 240;
}
#define UI_PX(v)  ui_px(v)
