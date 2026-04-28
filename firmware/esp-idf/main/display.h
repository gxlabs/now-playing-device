#pragma once

#include <stdint.h>

void display_init(void);
void display_set_backlight(uint8_t duty);  /* 0–255, fades smoothly */
