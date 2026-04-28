#include "display.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lvgl_port.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "display";

/* Pin definitions — Seeed XIAO Round Display */
#define PIN_CLK     GPIO_NUM_19
#define PIN_MOSI    GPIO_NUM_18
#define PIN_MISO    GPIO_NUM_20
#define PIN_CS      GPIO_NUM_1
#define PIN_DC      GPIO_NUM_21
#define PIN_RST     GPIO_NUM_0
#define PIN_BL      GPIO_NUM_2

#define LCD_H_RES   240
#define LCD_V_RES   240
#define SPI_HOST_ID SPI2_HOST

#define BL_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_FADE_MS      400

static void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = BL_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t ch = {
        .gpio_num = PIN_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER_0,
        .duty = 255,
        .hpoint = 0,
    };
    ledc_channel_config(&ch);

    ledc_fade_func_install(0);
}

void display_set_backlight(uint8_t duty)
{
    ledc_set_fade_with_time(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty, BL_FADE_MS);
    ledc_fade_start(BL_LEDC_MODE, BL_LEDC_CHANNEL, LEDC_FADE_NO_WAIT);
}

void display_init(void)
{
    backlight_init();

    /* SPI bus */
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST_ID, &bus, SPI_DMA_CH_AUTO));

    /* LCD panel IO (SPI) */
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI_HOST_ID, &io_cfg, &io));

    /* GC9A01A panel — no mirror/swap, LVGL handles rotation */
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    ESP_LOGI(TAG, "GC9A01A initialized");

    /* LVGL port */
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    /* Use LVGL software rotation for 90° CCW + mirror fix */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * 60 * sizeof(uint16_t),
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .flags = {
            .swap_bytes = 1,
            .sw_rotate = 1,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);

    /* 270° = 90° counter-clockwise */
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);

    ESP_LOGI(TAG, "LVGL display ready (sw rotation 270)");
}
