#pragma once

/* Waveshare ESP32-S3-Touch-LCD-1.46 board plumbing shared by the display and
   touch drivers: one I2C bus (the SPD2010 touch, the QMI8658 IMU and the
   PCF85063 RTC all hang off it) and the TCA9554 expander that owns both reset
   lines. */

#include "driver/i2c_master.h"

/* Which way up the screen runs. The panel's native raster puts the top of the
   image at the end the USB-C connector is on, which is Waveshare's demo
   orientation (cable at the bottom of the picture). Flipping 180 degrees puts
   the cable at the top, which is how this thing is meant to sit on a desk.
   The panel and the touch panel flip together. */
#define BOARD_FLIP_180  1

/* Shared I2C master bus (SDA GPIO11 / SCL GPIO10). Created on first call. */
i2c_master_bus_handle_t board_i2c_bus(void);

/* Pulse LCD_RST (EXIO2) and TP_RST (EXIO1) low then high via the TCA9554, and
   park SD_CS (EXIO3) high so the idle card doesn't answer on the SPI bus.
   Must run before the panel and touch drivers are created. */
void board_reset_peripherals(void);
