#pragma once

#include "now_playing.h"
#include <stdint.h>

void ui_init(void);
void ui_update_from_state(const np_state_t *state, const uint8_t *art_pixels, int art_size);
