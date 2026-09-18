#include "ws_s3_bus.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

#define PIN_SDA  GPIO_NUM_11
#define PIN_SCL  GPIO_NUM_10

/* TCA9554PWR. A0-A2 are strapped low on this board, so 0x20; the pin-compatible
   TCA9554A answers on 0x38 instead, so probe for whichever is fitted. */
#define TCA9554_ADDR   0x20
#define TCA9554A_ADDR  0x38

#define TCA9554_REG_OUTPUT  0x01
#define TCA9554_REG_CONFIG  0x03

/* Expander bits. Waveshare's "EXIOn" labels are 1-based, the chip's pins are
   P0-P7, so EXIO1 is P0. */
#define EXIO_TP_RST   (1 << 0)   /* EXIO1 */
#define EXIO_LCD_RST  (1 << 1)   /* EXIO2 */
#define EXIO_SD_CS    (1 << 2)   /* EXIO3 */
/* P3/P4 (EXIO4/EXIO5) are the IMU interrupt outputs — leave them as inputs. */
#define EXIO_OUTPUTS  (EXIO_TP_RST | EXIO_LCD_RST | EXIO_SD_CS)

static i2c_master_bus_handle_t s_bus;

i2c_master_bus_handle_t board_i2c_bus(void)
{
    if (s_bus) return s_bus;

    i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_bus));
    return s_bus;
}

static void expander_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    ESP_ERROR_CHECK(i2c_master_transmit(dev, buf, sizeof(buf), pdMS_TO_TICKS(100)));
}

void board_reset_peripherals(void)
{
    i2c_master_bus_handle_t bus = board_i2c_bus();

    uint8_t addr = TCA9554_ADDR;
    if (i2c_master_probe(bus, addr, pdMS_TO_TICKS(50)) != ESP_OK) {
        addr = TCA9554A_ADDR;
        ESP_ERROR_CHECK(i2c_master_probe(bus, addr, pdMS_TO_TICKS(50)));
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &dev));

    /* Drive the levels before switching the pins to outputs, so the panel and
       touch controller see a clean low rather than a glitch. */
    expander_write(dev, TCA9554_REG_OUTPUT, EXIO_SD_CS);
    expander_write(dev, TCA9554_REG_CONFIG, (uint8_t)~EXIO_OUTPUTS);
    vTaskDelay(pdMS_TO_TICKS(20));
    expander_write(dev, TCA9554_REG_OUTPUT, EXIO_OUTPUTS);
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "TCA9554 @ 0x%02X — LCD/touch out of reset", addr);
}
