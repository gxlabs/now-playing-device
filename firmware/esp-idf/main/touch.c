#include "touch.h"
#include "ui.h"

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "touch";

#define CHSC6X_ADDR  0x2E
#define PIN_SDA      GPIO_NUM_22
#define PIN_SCL      GPIO_NUM_23
#define PIN_INT      GPIO_NUM_17

static i2c_master_dev_handle_t s_dev;

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint8_t buf[5] = {0};
    esp_err_t ret = i2c_master_receive(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(10));
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
    /* I2C bus */
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

    /* CHSC6X touch device */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CHSC6X_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev));

    /* Register as LVGL input device */
    if (lvgl_port_lock(0)) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, read_cb);
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "CHSC6X touch ready (0x%02X)", CHSC6X_ADDR);
}
