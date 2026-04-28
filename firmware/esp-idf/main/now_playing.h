#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool playing;
    char title[128];
    char artist[128];
    char album[128];
    float duration;
    float elapsed;
    float playback_rate;
    char track_id[64];
    char artwork_id[16];
    uint32_t fetch_time_ms;
} np_state_t;

void now_playing_start(void);
void now_playing_send_command(const char *action);
