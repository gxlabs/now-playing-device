#include "touch.h"
#include "ui.h"
#include "serial.h"

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* M5Stack Dial input: FT3267 capacitive touch (I2C), rotary encoder and the
   encoder push button.

     touch    tap the on-screen prev / play / next buttons, as on other boards
     rotate   next / previous track
     press    play / pause                                               */

static const char *TAG = "touch";

#define FT_ADDR      0x38
#define PIN_SDA      GPIO_NUM_11
#define PIN_SCL      GPIO_NUM_12

#define PIN_ENC_A    GPIO_NUM_40
#define PIN_ENC_B    GPIO_NUM_41
#define PIN_BTN      GPIO_NUM_42

/* PCNT counts every edge of both channels (4 per full quadrature cycle); the
   Dial's detents are 4 counts apart. Tune here if a click skips or doubles. */
#define COUNTS_PER_STEP  4

static i2c_master_dev_handle_t s_dev;

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    /* FT3267 register map: 0x02 = touch count, 0x03.. = XH XL YH YL */
    const uint8_t reg = 0x02;
    uint8_t buf[5] = {0};
    esp_err_t ret = i2c_master_transmit_receive(s_dev, &reg, 1, buf, sizeof(buf),
                                                pdMS_TO_TICKS(10));
    bool pressed = (ret == ESP_OK && (buf[0] & 0x0F) > 0);

    /* A press that wakes the screen from dim is swallowed, so lighting the
       display doesn't also hit prev/play/next. */
    static bool s_swallow = false;
    if (pressed) {
        if (ui_mark_activity()) s_swallow = true;
    } else {
        s_swallow = false;
    }

    if (pressed && !s_swallow) {
        data->point.x = ((uint16_t)(buf[1] & 0x0F) << 8) | buf[2];
        data->point.y = ((uint16_t)(buf[3] & 0x0F) << 8) | buf[4];
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void encoder_task(void *arg)
{
    pcnt_unit_handle_t unit = arg;
    int last = 0;
    bool btn_prev = false;
    int btn_ms = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));

        int count = 0;
        pcnt_unit_get_count(unit, &count);
        int steps = (count - last) / COUNTS_PER_STEP;
        if (steps != 0) {
            last += steps * COUNTS_PER_STEP;
            /* Any turn wakes the screen; the turn itself still acts, since
               there is nothing on the encoder to hit by accident. */
            ui_mark_activity();
            for (int i = 0; i < (steps < 0 ? -steps : steps); i++) {
                serial_send_command(steps > 0 ? "next" : "previous");
            }
        }

        /* Button: act on release, with a 30 ms debounce. A press that wakes a
           dimmed screen only wakes it. */
        bool down = gpio_get_level(PIN_BTN) == 0;
        if (down) {
            btn_ms += 10;
        } else {
            if (btn_prev && btn_ms >= 30) {
                if (!ui_mark_activity()) serial_send_command("toggle");
            }
            btn_ms = 0;
        }
        btn_prev = down;
    }
}

static void encoder_init(void)
{
    pcnt_unit_config_t ucfg = {
        .low_limit = -30000,
        .high_limit = 30000,
    };
    pcnt_unit_handle_t unit;
    ESP_ERROR_CHECK(pcnt_new_unit(&ucfg, &unit));

    pcnt_glitch_filter_config_t filt = { .max_glitch_ns = 1000 };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(unit, &filt));

    pcnt_chan_config_t ca = { .edge_gpio_num = PIN_ENC_A, .level_gpio_num = PIN_ENC_B };
    pcnt_chan_config_t cb = { .edge_gpio_num = PIN_ENC_B, .level_gpio_num = PIN_ENC_A };
    pcnt_channel_handle_t cha, chb;
    ESP_ERROR_CHECK(pcnt_new_channel(unit, &ca, &cha));
    ESP_ERROR_CHECK(pcnt_new_channel(unit, &cb, &chb));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(cha,
        PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(cha,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chb,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chb,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    ESP_ERROR_CHECK(pcnt_unit_enable(unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(unit));
    ESP_ERROR_CHECK(pcnt_unit_start(unit));

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << PIN_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));

    xTaskCreate(encoder_task, "encoder", 3072, unit, 4, NULL);
    ESP_LOGI(TAG, "encoder ready (A=%d B=%d BTN=%d)", PIN_ENC_A, PIN_ENC_B, PIN_BTN);
}

void touch_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = FT_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev));

    if (lvgl_port_lock(0)) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, read_cb);
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "FT3267 touch ready (0x%02X)", FT_ADDR);

    encoder_init();
}
