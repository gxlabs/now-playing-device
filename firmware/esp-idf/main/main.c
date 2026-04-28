#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "display.h"
#include "touch.h"
#include "serial.h"
#include "ui.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Now Playing (USB serial) - starting");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    display_init();
    touch_init();
    ui_init();
    serial_init();
}
