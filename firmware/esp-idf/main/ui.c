#include "ui.h"
#include "serial.h"
#include "display.h"
#include "esp_lvgl_port.h"
#include "libs/qrcode/lv_qrcode.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ui";

#define S 240   /* screen size */
#define SETUP_URL "https://static.gxlabs.co/now-playing"

/* ── Widgets ──────────────────────────────────────────────────── */

/* Setup screen (shown until first data arrives) */
static lv_obj_t *setup_screen;
static lv_obj_t *qr_widget;

/* Now-playing screen */
static lv_obj_t *art_img;
static lv_obj_t *fade;       /* gradient strip above overlay */
static lv_obj_t *overlay;
static lv_obj_t *title_label;
static lv_obj_t *artist_label;
static lv_obj_t *elapsed_label;
static lv_obj_t *remaining_label;
static lv_obj_t *progress_bar;
static lv_obj_t *btn_prev;
static lv_obj_t *btn_toggle;
static lv_obj_t *btn_next;
static lv_obj_t *toggle_label;
static lv_obj_t *idle_label;

static lv_image_dsc_t art_dsc;

/* State */
static float s_elapsed, s_rate, s_duration;
static float s_last_play_rate = 1.0f;   /* remembers rate so resume matches it */
static uint32_t s_anchor_ms;
static uint32_t s_optimistic_until_ms;  /* ignore server-reported play state until this tick */
static bool s_has_data;
static bool s_connected;   /* true after first state update */

#define OPTIMISTIC_HOLD_MS 1500

/* Idle dim — the Mac pushes state every second whether or not anything
   changed, so "activity" specifically means a track or play/pause transition,
   or a touch press. Pure same-track playback doesn't count. */
#define IDLE_TIMEOUT_MS (5 * 60 * 1000)
#define BL_FULL  255
#define BL_DIM   20

static uint32_t s_last_activity_ms;
static bool s_dimmed;
static char s_prev_track_id[64];
static bool s_prev_was_playing;

/* Server presence: if we don't receive a state frame for a while, the Mac-side
   app isn't running. Revert to the setup QR so the user knows. */
#define DISCONNECT_TIMEOUT_MS 8000
static uint32_t s_last_state_ms;

/* ── Helpers ──────────────────────────────────────────────────── */

static void fmt_time(char *buf, int size, float secs)
{
    if (secs < 0) secs = 0;
    int s = (int)secs;
    int m = s / 60;
    s %= 60;
    snprintf(buf, size, "%d:%02d", m, s);
}

/* ── Idle dim ─────────────────────────────────────────────────── */

bool ui_mark_activity(void)
{
    s_last_activity_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (s_dimmed) {
        s_dimmed = false;
        display_set_backlight(BL_FULL);
        return true;
    }
    return false;
}

static void idle_check_cb(lv_timer_t *t)
{
    if (s_dimmed) return;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if ((int32_t)(now - s_last_activity_ms) > IDLE_TIMEOUT_MS) {
        s_dimmed = true;
        display_set_backlight(BL_DIM);
    }
}

/* ── Disconnect watchdog ──────────────────────────────────────────
   Mac pushes state every second; an 8s gap means the app isn't running. */

static void disconnect_check_cb(lv_timer_t *t)
{
    if (!s_connected) return;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if ((int32_t)(now - s_last_state_ms) > DISCONNECT_TIMEOUT_MS) {
        s_connected = false;
        s_has_data = false;
        lv_obj_remove_flag(setup_screen, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "disconnected — showing setup QR");
    }
}

/* ── Progress timer ───────────────────────────────────────────── */

static void progress_timer_cb(lv_timer_t *t)
{
    if (!s_has_data || s_duration <= 0) return;

    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    float dt = (float)(now - s_anchor_ms) / 1000.0f;
    float e = s_elapsed + dt * s_rate;
    if (e < 0) e = 0;
    if (e > s_duration) e = s_duration;

    lv_bar_set_value(progress_bar, (int)(e / s_duration * 1000.0f), LV_ANIM_OFF);

    char buf[16];
    fmt_time(buf, sizeof(buf), e);
    lv_label_set_text(elapsed_label, buf);

    char rbuf[16];
    rbuf[0] = '-';
    fmt_time(rbuf + 1, sizeof(rbuf) - 1, s_duration - e);
    lv_label_set_text(remaining_label, rbuf);
}

/* ── Touch handlers ───────────────────────────────────────────── */

static void on_prev(lv_event_t *ev)    { serial_send_command("previous"); }
static void on_next(lv_event_t *ev)    { serial_send_command("next"); }

/* Optimistic toggle: flip the play/pause icon and freeze/resume progress
   locally before the server confirms — feels instant. The next state update
   from the Mac will overwrite this if it diverges. */
static void on_toggle(lv_event_t *ev)
{
    if (s_has_data) {
        bool was_playing = s_rate > 0.0f;
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        float dt = (float)(now - s_anchor_ms) / 1000.0f;
        float local_e = s_elapsed + dt * s_rate;
        if (local_e < 0) local_e = 0;
        if (s_duration > 0 && local_e > s_duration) local_e = s_duration;
        s_elapsed = local_e;
        s_anchor_ms = now;
        s_rate = was_playing ? 0.0f : s_last_play_rate;
        lv_label_set_text(toggle_label,
            was_playing ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
        s_optimistic_until_ms = now + OPTIMISTIC_HOLD_MS;
    }
    serial_send_command("toggle");
}

/* ── Button helper ────────────────────────────────────────────── */

static lv_obj_t *make_btn(lv_obj_t *parent, const char *sym, int sz,
                           lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, sz, sz);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, 70, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, sym);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);
    return btn;
}

/* ── Setup screen (QR code) ───────────────────────────────────── */

static void create_setup_screen(lv_obj_t *scr)
{
    setup_screen = lv_obj_create(scr);
    lv_obj_set_size(setup_screen, S, S);
    lv_obj_set_pos(setup_screen, 0, 0);
    lv_obj_set_style_bg_color(setup_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(setup_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(setup_screen, 0, 0);
    lv_obj_set_style_pad_all(setup_screen, 0, 0);
    lv_obj_remove_flag(setup_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *lbl = lv_label_create(setup_screen);
    lv_label_set_text(lbl, "Now Playing");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 30);

    /* QR code */
    qr_widget = lv_qrcode_create(setup_screen);
    lv_qrcode_set_size(qr_widget, 100);
    lv_qrcode_set_dark_color(qr_widget, lv_color_white());
    lv_qrcode_set_light_color(qr_widget, lv_color_black());
    lv_qrcode_update(qr_widget, SETUP_URL, strlen(SETUP_URL));
    lv_obj_center(qr_widget);

    /* URL caption directly under the QR */
    lv_obj_t *url = lv_label_create(setup_screen);
    lv_label_set_text(url, "static.gxlabs.co/now-playing");
    lv_obj_set_style_text_color(url, lv_color_make(180, 180, 180), 0);
    lv_obj_set_style_text_font(url, &lv_font_montserrat_12, 0);
    lv_obj_align(url, LV_ALIGN_BOTTOM_MID, 0, -55);
}

/* ── Now-playing screen ───────────────────────────────────────── */

static void create_playing_screen(lv_obj_t *scr)
{
    /* Full-screen album art */
    art_img = lv_image_create(scr);
    lv_obj_set_size(art_img, S, S);
    lv_obj_set_pos(art_img, 0, 0);
    lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN);

    /* Soft fade strip — blurs the boundary between album art and overlay */
    fade = lv_obj_create(scr);
    lv_obj_set_size(fade, S, 56);
    lv_obj_align(fade, LV_ALIGN_BOTTOM_MID, 0, -146);  /* sits just above overlay */
    lv_obj_set_style_bg_color(fade, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(fade, 0, 0);
    lv_obj_set_style_bg_grad_color(fade, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_opa(fade, 170, 0);
    lv_obj_set_style_bg_grad_dir(fade, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(fade, 0, 0);
    lv_obj_set_style_radius(fade, 0, 0);
    lv_obj_set_style_pad_all(fade, 0, 0);
    lv_obj_remove_flag(fade, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(fade, LV_OBJ_FLAG_HIDDEN);

    /* Semi-transparent overlay */
    overlay = lv_obj_create(scr);
    lv_obj_set_size(overlay, S, 138);
    lv_obj_align(overlay, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, 170, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);

    /* Title */
    title_label = lv_label_create(overlay);
    lv_obj_set_width(title_label, S - 4);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 8);
    lv_label_set_text(title_label, "");
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_SCROLL);

    /* Artist */
    artist_label = lv_label_create(overlay);
    lv_obj_set_width(artist_label, 200);
    lv_obj_align(artist_label, LV_ALIGN_TOP_MID, 0, 32);
    lv_label_set_text(artist_label, "");
    lv_obj_set_style_text_color(artist_label, lv_color_make(200, 200, 200), 0);
    lv_obj_set_style_text_font(artist_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(artist_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(artist_label, LV_LABEL_LONG_SCROLL);

    /* Progress row: elapsed (fixed-width) | bar | remaining (fixed-width)
       — fixed widths reserve space for the longest expected times so the
       bar can sit between them with consistent 1px padding. */
    elapsed_label = lv_label_create(overlay);
    lv_obj_set_width(elapsed_label, 36);
    lv_obj_align(elapsed_label, LV_ALIGN_TOP_LEFT, 20, 52);
    lv_label_set_text(elapsed_label, "0:00");
    lv_obj_set_style_text_color(elapsed_label, lv_color_make(180, 180, 180), 0);
    lv_obj_set_style_text_font(elapsed_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(elapsed_label, LV_TEXT_ALIGN_RIGHT, 0);

    progress_bar = lv_bar_create(overlay);
    lv_obj_set_size(progress_bar, 120, 5);
    lv_obj_align(progress_bar, LV_ALIGN_TOP_LEFT, 59, 57);
    lv_bar_set_range(progress_bar, 0, 1000);
    lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(progress_bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(progress_bar, 80, LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress_bar, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(progress_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(progress_bar, 3, LV_PART_INDICATOR);

    remaining_label = lv_label_create(overlay);
    lv_obj_set_width(remaining_label, 42);
    lv_obj_align(remaining_label, LV_ALIGN_TOP_LEFT, 182, 52);
    lv_label_set_text(remaining_label, "-0:00");
    lv_obj_set_style_text_color(remaining_label, lv_color_make(180, 180, 180), 0);
    lv_obj_set_style_text_font(remaining_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(remaining_label, LV_TEXT_ALIGN_LEFT, 0);

    /* Control buttons — large, well-spaced */
    btn_prev = make_btn(overlay, LV_SYMBOL_PREV, 38, on_prev);
    lv_obj_align(btn_prev, LV_ALIGN_BOTTOM_MID, -60, -19);

    btn_toggle = make_btn(overlay, LV_SYMBOL_PLAY, 60, on_toggle);
    lv_obj_align(btn_toggle, LV_ALIGN_BOTTOM_MID, 0, -8);
    toggle_label = lv_obj_get_child(btn_toggle, 0);

    btn_next = make_btn(overlay, LV_SYMBOL_NEXT, 38, on_next);
    lv_obj_align(btn_next, LV_ALIGN_BOTTOM_MID, 60, -19);

    /* Idle label (nothing playing but connected) */
    idle_label = lv_label_create(scr);
    lv_obj_center(idle_label);
    lv_label_set_text(idle_label, "Nothing playing");
    lv_obj_set_style_text_color(idle_label, lv_color_make(120, 120, 120), 0);
    lv_obj_set_style_text_font(idle_label, &lv_font_montserrat_16, 0);
    lv_obj_add_flag(idle_label, LV_OBJ_FLAG_HIDDEN);
}

/* ── Public API ───────────────────────────────────────────────── */

void ui_init(void)
{
    if (!lvgl_port_lock(0)) return;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    create_playing_screen(scr);
    create_setup_screen(scr);   /* on top initially */

    lv_timer_create(progress_timer_cb, 100, NULL);

    s_last_activity_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    lv_timer_create(idle_check_cb, 1000, NULL);
    lv_timer_create(disconnect_check_cb, 1000, NULL);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "UI ready (showing setup QR)");
}

void ui_update_from_state(const np_state_t *state, const uint8_t *art_pixels,
                          int art_size)
{
    if (!lvgl_port_lock(pdMS_TO_TICKS(200))) return;

    s_last_state_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    /* First data received (or reconnect after a disconnect) — dismiss QR */
    if (!s_connected) {
        s_connected = true;
        lv_obj_add_flag(setup_screen, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "connected — hiding setup QR");
    }

    /* Detect meaningful change for the idle dim timer. The Mac pushes state
       every second, so we only count track changes and play/pause transitions
       — not steady-state playback of the same track. */
    bool now_playing_active = state->playing && state->playback_rate > 0;
    bool track_changed = strncmp(s_prev_track_id, state->track_id,
                                 sizeof(s_prev_track_id)) != 0;
    if (track_changed || s_prev_was_playing != now_playing_active) {
        ui_mark_activity();
    }
    strncpy(s_prev_track_id, state->track_id, sizeof(s_prev_track_id) - 1);
    s_prev_track_id[sizeof(s_prev_track_id) - 1] = '\0';
    s_prev_was_playing = now_playing_active;

    if (!state->playing) {
        lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(fade, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(idle_label, LV_OBJ_FLAG_HIDDEN);
        s_has_data = false;
        lvgl_port_unlock();
        return;
    }

    lv_obj_add_flag(idle_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(art_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(fade, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);

    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    /* Honor the optimistic toggle for a moment so the UI doesn't flicker back
       to the server's stale "still playing" snapshot before nowplaying-cli
       has caught up. */
    bool optimistic_hold = (int32_t)(s_optimistic_until_ms - now_ms) > 0;

    lv_label_set_text(title_label, state->title);
    lv_label_set_text(artist_label, state->artist);
    if (!optimistic_hold) {
        lv_label_set_text(toggle_label,
            state->playback_rate > 0 ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        if (state->playback_rate > 0) s_last_play_rate = state->playback_rate;
    }

    if (art_pixels) {
        art_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        art_dsc.header.w = art_size;
        art_dsc.header.h = art_size;
        art_dsc.data = art_pixels;
        art_dsc.data_size = art_size * art_size * 2;
        lv_image_set_src(art_img, &art_dsc);
    }

    /* Only reset the interpolation anchor when there's a meaningful
       change (track change, seek, rate change).  Otherwise let the
       local interpolation run smoothly to avoid flicker. */
    s_duration = state->duration;

    if (!s_has_data) {
        /* First update — hard reset */
        s_elapsed = state->elapsed;
        s_anchor_ms = state->fetch_time_ms;
        s_rate = state->playback_rate;
    } else if (state->playback_rate != s_rate) {
        /* Rate disagrees. While optimistic_hold is active, wait for the
           server to catch up to our toggle; otherwise treat as a genuine
           change and hard reset. */
        if (!optimistic_hold) {
            s_elapsed = state->elapsed;
            s_anchor_ms = state->fetch_time_ms;
            s_rate = state->playback_rate;
        }
    } else {
        /* Rates match — keep elapsed in sync with the server even during
           the optimistic hold so the displayed time stays accurate. */
        float dt = (float)(now_ms - s_anchor_ms) / 1000.0f;
        float local_e = s_elapsed + dt * s_rate;
        float drift = state->elapsed - local_e;

        if (drift > 2.0f || drift < -2.0f) {
            /* Seek or large drift — hard reset */
            s_elapsed = state->elapsed;
            s_anchor_ms = state->fetch_time_ms;
        }
        /* else: keep current anchor, it's close enough */
    }

    s_has_data = true;

    lvgl_port_unlock();
}
