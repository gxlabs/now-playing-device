#include "serial.h"
#include "ui.h"
#include "now_playing.h"

#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

static const char *TAG = "serial";

#define ART_SIZE    240
#define ART_BUF_SZ  (ART_SIZE * ART_SIZE * 2)   /* RGB565 */
#define JSON_BUF_SZ 1024
#define RX_BUF_SZ   16384
#define TX_BUF_SZ   256

static np_state_t s_state;
static uint8_t *s_art_buf;
static char *s_json_buf;

/* ── Helpers ──────────────────────────────────────────────────── */

static bool read_exact(uint8_t *buf, int len, int timeout_ms)
{
    int total = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (total < len) {
        TickType_t remaining = deadline - xTaskGetTickCount();
        if ((int32_t)remaining <= 0) return false;
        int n = usb_serial_jtag_read_bytes(buf + total, len - total, remaining);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

static void parse_state(const char *json, int len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return;

    s_state.playing = cJSON_IsTrue(cJSON_GetObjectItem(root, "playing"));

    if (s_state.playing) {
        cJSON *j;
        #define S(f, k) if ((j = cJSON_GetObjectItem(root, k)) && j->valuestring) \
                            strncpy(s_state.f, j->valuestring, sizeof(s_state.f) - 1);
        #define N(f, k) if ((j = cJSON_GetObjectItem(root, k))) \
                            s_state.f = (float)j->valuedouble;

        S(title, "title")      S(artist, "artist")
        S(album, "album")      S(track_id, "trackId")
        S(artwork_id, "artworkId")
        N(duration, "duration") N(elapsed, "elapsed")
        N(playback_rate, "playbackRate")

        #undef S
        #undef N

        s_state.fetch_time_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    }

    cJSON_Delete(root);

    /* Update UI with state (artwork handled separately by 0x02 frame) */
    ui_update_from_state(&s_state, NULL, ART_SIZE);
}

/* ── Reader task ──────────────────────────────────────────────── */

static void serial_task(void *arg)
{
    ESP_LOGI(TAG, "waiting for data...");

    while (1) {
        uint8_t hdr;
        int n = usb_serial_jtag_read_bytes(&hdr, 1, pdMS_TO_TICKS(100));
        if (n <= 0) continue;

        if (hdr == 0x00) {
            /* Ping — identify ourselves */
            const char *ack = "NP:ACK\n";
            usb_serial_jtag_write_bytes((const uint8_t *)ack, 7, pdMS_TO_TICKS(100));
            continue;

        } else if (hdr == 0x01) {
            /* State: 2-byte BE length + JSON */
            uint8_t lb[2];
            if (!read_exact(lb, 2, 1000)) continue;
            int len = (lb[0] << 8) | lb[1];
            if (len <= 0 || len >= JSON_BUF_SZ) continue;
            if (!read_exact((uint8_t *)s_json_buf, len, 2000)) continue;
            s_json_buf[len] = '\0';
            parse_state(s_json_buf, len);

        } else if (hdr == 0x02) {
            /* Artwork: 4-byte BE length + RGB565 pixels */
            uint8_t lb[4];
            if (!read_exact(lb, 4, 1000)) continue;
            int len = (lb[0] << 24) | (lb[1] << 16) | (lb[2] << 8) | lb[3];
            if (len != ART_BUF_SZ) {
                ESP_LOGW(TAG, "bad art size %d (want %d)", len, ART_BUF_SZ);
                /* drain */
                uint8_t tmp;
                for (int i = 0; i < len && read_exact(&tmp, 1, 500); i++);
                continue;
            }
            if (!read_exact(s_art_buf, len, 15000)) {
                ESP_LOGW(TAG, "artwork read timeout");
                continue;
            }
            ESP_LOGI(TAG, "artwork received");
            ui_update_from_state(&s_state, s_art_buf, ART_SIZE);
        }
    }
}

/* ── Public ───────────────────────────────────────────────────── */

void serial_init(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = RX_BUF_SZ,
        .tx_buffer_size = TX_BUF_SZ,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));

    s_json_buf = heap_caps_malloc(JSON_BUF_SZ, MALLOC_CAP_DEFAULT);
    s_art_buf  = heap_caps_malloc(ART_BUF_SZ, MALLOC_CAP_DEFAULT);
    assert(s_json_buf && s_art_buf);

    xTaskCreate(serial_task, "serial_rx", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "USB serial ready");
}

void serial_send_command(const char *action)
{
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "CMD:%s\n", action);
    usb_serial_jtag_write_bytes((uint8_t *)buf, len, pdMS_TO_TICKS(100));
}
