#include "display.h"
#include "board.h"
#include "ws_s3_bus.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_spd2010.h"
#include "esp_lvgl_port.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "display";

/* Pin definitions — Waveshare ESP32-S3-Touch-LCD-1.46 (QSPI).
   LCD_RST hangs off the TCA9554, not a GPIO — see ws_s3_bus.c. */
#define PIN_CLK   GPIO_NUM_40
#define PIN_D0    GPIO_NUM_46
#define PIN_D1    GPIO_NUM_45
#define PIN_D2    GPIO_NUM_42
#define PIN_D3    GPIO_NUM_41
#define PIN_CS    GPIO_NUM_21
#define PIN_BL    GPIO_NUM_5

#define LCD_H_RES   UI_SIZE
#define LCD_V_RES   UI_SIZE
#define SPI_HOST_ID SPI2_HOST

/* Lines per LVGL draw buffer. Two of these live in internal DMA-capable RAM;
   the artwork frame itself sits in PSRAM. */
#define DRAW_LINES  40

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

    /* Starts dark — main() brings it up once the first frame has been drawn,
       so the panel's power-on garbage never reaches the eye. */
    ledc_channel_config_t ch = {
        .gpio_num = PIN_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
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

/* The SPD2010 only accepts window commands on 4-pixel boundaries, so every
   area LVGL asks us to flush has to be widened out to one. 412 is itself a
   multiple of 4, so rounding never runs off the edge of the panel. */
static void rounder_cb(lv_area_t *area)
{
    area->x1 &= ~3;
    area->y1 &= ~3;
    area->x2 |= 3;
    area->y2 |= 3;
}

void display_init(void)
{
    backlight_init();
    board_reset_peripherals();

    /* QSPI bus */
    const spi_bus_config_t bus = SPD2010_PANEL_BUS_QSPI_CONFIG(
        PIN_CLK, PIN_D0, PIN_D1, PIN_D2, PIN_D3,
        LCD_H_RES * DRAW_LINES * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST_ID, &bus, SPI_DMA_CH_AUTO));

    /* LCD panel IO (QSPI) */
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = SPD2010_PANEL_IO_QSPI_CONFIG(PIN_CS, NULL, NULL);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI_HOST_ID, &io_cfg, &io));

    /* SPD2010 panel. Reset was already released over the expander. */
    const spd2010_vendor_config_t vendor_cfg = {
        .flags = { .use_qspi_interface = 1 },
    };
    esp_lcd_panel_handle_t panel;
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_spd2010(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, BOARD_FLIP_180, BOARD_FLIP_180));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    ESP_LOGI(TAG, "SPD2010 initialized (%dx%d QSPI)", LCD_H_RES, LCD_V_RES);

    /* LVGL port */
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * DRAW_LINES,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .rounder_cb = rounder_cb,
        .flags = {
            .buff_dma = 1,
            .swap_bytes = 1,
        },
    };
    if (!lvgl_port_add_disp(&disp_cfg)) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        abort();
    }

    ESP_LOGI(TAG, "LVGL display ready");
}
