#pragma once

#include "now_playing.h"
#include <stdbool.h>
#include <stdint.h>

void ui_init(void);
void ui_update_from_state(const np_state_t *state, const uint8_t *art_pixels, int art_size);

/* Bumps the activity timer and undims the backlight if needed.
   Returns true iff the call woke the screen from a dimmed state — the touch
   layer uses this to swallow the press that triggered the wake. */
bool ui_mark_activity(void);
