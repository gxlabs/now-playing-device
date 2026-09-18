#include "touch.h"
#include "ui.h"
#include "board.h"
#include "ws_s3_bus.h"

#include "esp_lvgl_port.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

/* SPD2010 capacitive touch, spoken directly over i2c_master.

   Espressif ship an esp_lcd_touch_spd2010 component, but it drives the part
   through esp_lcd_panel_io_i2c with a zero-length command phase; on ESP-IDF
   5.5 that turns into a zero-length i2c_master write before every read, which
   the new I2C driver rejects outright (ESP_ERR_INVALID_ARG). The register
   sequence below is ported from that component (Apache-2.0) onto plain
   i2c_master transactions.

   Every exchange is either a 4-byte write, or a 2-byte register address
   followed by a separate read — the controller wants a STOP between the two,
   so these stay as two transactions rather than a repeated start.

   TP_RST hangs off the TCA9554 and is released in board_reset_peripherals().
   TP_INT (GPIO4) is left alone: polling from the LVGL task keeps I2C traffic
   off the interrupt path. */

static const char *TAG = "touch";

#define SPD2010_ADDR     0x53
#define I2C_SPEED_HZ     100000
#define I2C_TIMEOUT_MS   100

/* 4-byte header + up to 10 fingers * 6 bytes */
#define HDP_BUF_SZ       64

static i2c_master_dev_handle_t s_dev;

typedef struct {
    bool pt_exist;
    bool gesture;
    bool aux;
    bool cpu_run;
    bool tic_in_cpu;
    bool tic_in_bios;
    uint16_t read_len;
} tp_status_t;

/* ── Register access ──────────────────────────────────────────── */

static esp_err_t tp_write(const uint8_t *data, size_t len)
{
    esp_err_t ret = i2c_master_transmit(s_dev, data, len, I2C_TIMEOUT_MS);
    esp_rom_delay_us(200);
    return ret;
}

static esp_err_t tp_read(uint8_t addr_lo, uint8_t addr_hi, uint8_t *buf, size_t len)
{
    const uint8_t addr[2] = { addr_lo, addr_hi };
    ESP_RETURN_ON_ERROR(tp_write(addr, sizeof(addr)), TAG, "addr write failed");
    esp_err_t ret = i2c_master_receive(s_dev, buf, len, I2C_TIMEOUT_MS);
    esp_rom_delay_us(200);
    return ret;
}

static esp_err_t tp_cmd(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
    const uint8_t cmd[4] = { b0, b1, b2, b3 };
    return tp_write(cmd, sizeof(cmd));
}

#define cmd_point_mode()  tp_cmd(0x50, 0x00, 0x00, 0x00)
#define cmd_touch_start() tp_cmd(0x46, 0x00, 0x00, 0x00)
#define cmd_cpu_start()   tp_cmd(0x04, 0x00, 0x01, 0x00)
#define cmd_clear_int()   tp_cmd(0x02, 0x00, 0x01, 0x00)

static esp_err_t read_status(tp_status_t *st)
{
    uint8_t d[4];
    ESP_RETURN_ON_ERROR(tp_read(0x20, 0x00, d, sizeof(d)), TAG, "status read failed");

    st->pt_exist    = d[0] & 0x01;
    st->gesture     = d[0] & 0x02;
    st->aux         = d[0] & 0x08;
    st->cpu_run     = d[1] & 0x08;
    st->tic_in_cpu  = d[1] & 0x20;
    st->tic_in_bios = d[1] & 0x40;
    st->read_len    = (uint16_t)(d[3] << 8 | d[2]);
    return ESP_OK;
}

/* Pulls one packet of touch points. Reports only the first finger — this UI
   has nothing multi-touch about it. */
static esp_err_t read_points(const tp_status_t *st, bool *pressed, uint16_t *x, uint16_t *y)
{
    uint8_t d[HDP_BUF_SZ];
    uint16_t len = st->read_len;
    if (len < 4 || len > sizeof(d)) return ESP_OK;   /* nothing usable */

    ESP_RETURN_ON_ERROR(tp_read(0x00, 0x03, d, len), TAG, "hdp read failed");

    /* d[4] is the first report's finger id; 0xF6 marks a gesture packet. */
    if (d[4] > 0x0A || !st->pt_exist) return ESP_OK;
    if ((len - 4) / 6 == 0) return ESP_OK;

    uint8_t weight = d[8];
    if (weight == 0) return ESP_OK;                  /* finger lifted */

    *x = (uint16_t)(((d[7] & 0xF0) << 4) | d[5]);
    *y = (uint16_t)(((d[7] & 0x0F) << 8) | d[6]);
    *pressed = true;
    return ESP_OK;
}

static esp_err_t read_hdp_status(uint8_t *status, uint16_t *next_len)
{
    uint8_t d[8];
    ESP_RETURN_ON_ERROR(tp_read(0xFC, 0x02, d, sizeof(d)), TAG, "hdp status failed");
    *status = d[5];
    *next_len = (uint16_t)(d[2] | d[3] << 8);
    return ESP_OK;
}

static esp_err_t drain_hdp(uint16_t len)
{
    uint8_t d[HDP_BUF_SZ];
    if (len == 0) return ESP_OK;
    if (len > sizeof(d)) len = sizeof(d);
    return tp_read(0x00, 0x03, d, len);
}

/* One poll of the controller's little state machine: it boots into a BIOS
   mode, has to be walked into CPU/point mode, and every packet has to be
   acknowledged before it will raise another. */
static esp_err_t poll(bool *pressed, uint16_t *x, uint16_t *y)
{
    tp_status_t st = {0};
    ESP_RETURN_ON_ERROR(read_status(&st), TAG, "status failed");

    if (st.tic_in_bios) {
        ESP_RETURN_ON_ERROR(cmd_clear_int(), TAG, "clear int failed");
        ESP_RETURN_ON_ERROR(cmd_cpu_start(), TAG, "cpu start failed");
    } else if (st.tic_in_cpu) {
        ESP_RETURN_ON_ERROR(cmd_point_mode(), TAG, "point mode failed");
        ESP_RETURN_ON_ERROR(cmd_touch_start(), TAG, "touch start failed");
        ESP_RETURN_ON_ERROR(cmd_clear_int(), TAG, "clear int failed");
    } else if (st.cpu_run && st.read_len == 0) {
        ESP_RETURN_ON_ERROR(cmd_clear_int(), TAG, "clear int failed");
    } else if (st.pt_exist || st.gesture) {
        ESP_RETURN_ON_ERROR(read_points(&st, pressed, x, y), TAG, "points failed");

        /* Keep draining until the controller says the packet is complete,
           bounded so a wedged part can't spin the LVGL task. */
        for (int i = 0; i < 8; i++) {
            uint8_t hdp_status;
            uint16_t next_len;
            ESP_RETURN_ON_ERROR(read_hdp_status(&hdp_status, &next_len), TAG, "hdp status failed");
            if (hdp_status == 0x82) {
                ESP_RETURN_ON_ERROR(cmd_clear_int(), TAG, "clear int failed");
                break;
            } else if (hdp_status == 0x00) {
                ESP_RETURN_ON_ERROR(drain_hdp(next_len), TAG, "drain failed");
            } else {
                break;
            }
        }
    } else if (st.cpu_run && st.aux) {
        ESP_RETURN_ON_ERROR(cmd_clear_int(), TAG, "clear int failed");
    }

    return ESP_OK;
}

/* ── LVGL input device ────────────────────────────────────────── */

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    bool pressed = false;
    uint16_t x = 0, y = 0;

    if (poll(&pressed, &x, &y) != ESP_OK) pressed = false;

    if (pressed) {
        if (x >= UI_SIZE) x = UI_SIZE - 1;
        if (y >= UI_SIZE) y = UI_SIZE - 1;
    }

#if BOARD_TOUCH_FLIP_180
    if (pressed) {
        x = UI_SIZE - 1 - x;
        y = UI_SIZE - 1 - y;
    }
#endif

    /* If a press wakes the screen from dim, swallow that whole press so the
       user doesn't accidentally hit prev/play/next while just trying to
       light the screen back up. */
    static bool s_swallow = false;
    if (pressed) {
        if (ui_mark_activity()) s_swallow = true;
    } else {
        s_swallow = false;
    }

    if (pressed && !s_swallow) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void touch_init(void)
{
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SPD2010_ADDR,
        .scl_speed_hz = I2C_SPEED_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(board_i2c_bus(), &dev_cfg, &s_dev));

    /* Announce ourselves: the first poll walks the controller out of BIOS
       mode, so a failure here is informative but not fatal. */
    uint8_t ver[18];
    if (tp_read(0x26, 0x00, ver, sizeof(ver)) == ESP_OK) {
        ESP_LOGI(TAG, "SPD2010 touch ready (fw %d.%d)", ver[5], ver[4]);
    } else {
        ESP_LOGW(TAG, "SPD2010 touch did not answer version read");
    }

    if (lvgl_port_lock(0)) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, read_cb);
        lvgl_port_unlock();
    }
}
