#include "touch.h"
#include "ui.h"

#include "freertos/FreeRTOS.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "touch";

#define CHSC6X_ADDR  0x2E
#define PIN_SDA      GPIO_NUM_22
#define PIN_SCL      GPIO_NUM_23
#define I2C_PORT     I2C_NUM_0

/* Uses the legacy `driver/i2c.h` API (polling/FSM-reset on bus-busy) instead
   of the new `driver/i2c_master.h` driver. The new driver's ISR has an
   unbounded `while(i2c_ll_is_bus_busy(...)){}` spin that occasionally hangs
   on CHSC6X reads and trips the interrupt watchdog (chip reboots → setup
   QR). Fixed in IDF 5.3+; legacy driver here works around it on 5.2.x. */

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint8_t buf[5] = {0};
    esp_err_t ret = i2c_master_read_from_device(I2C_PORT, CHSC6X_ADDR,
                                                buf, sizeof(buf),
                                                pdMS_TO_TICKS(10));
    bool pressed = (ret == ESP_OK && buf[0] > 0);

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
        data->point.x = ((uint16_t)buf[1] << 8) | buf[2];
        data->point.y = ((uint16_t)buf[3] << 8) | buf[4];
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void touch_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0));

    if (lvgl_port_lock(0)) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, read_cb);
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "CHSC6X touch ready (0x%02X)", CHSC6X_ADDR);
}
