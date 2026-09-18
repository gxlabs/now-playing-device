#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "board.h"
#include "display.h"
#include "touch.h"
#include "serial.h"
#include "ui.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Now Playing (USB serial) - starting on %s", BOARD_NAME);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    display_init();
    touch_init();
    ui_init();

    /* The backlight comes up dark; give LVGL a moment to put the setup screen
       on the panel so the first thing lit is the UI, not power-on noise. */
    vTaskDelay(pdMS_TO_TICKS(120));
    display_set_backlight(255);

    serial_init();
}
