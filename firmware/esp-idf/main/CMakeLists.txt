# Board support is picked by the build target:
#   esp32s3 -> Waveshare ESP32-S3-Touch-LCD-1.46 (SPD2010 412x412 QSPI)
#   esp32c6 -> Seeed XIAO ESP32-C6 + Round Display (GC9A01A 240x240 SPI)
# Fonts are sized to match the panel — see board.h.
if(CONFIG_IDF_TARGET_ESP32S3)
    set(BOARD_SRCS
        "boards/ws_s3_bus.c"
        "boards/ws_s3_display.c"
        "boards/ws_s3_touch.c")
    set(FONT_SRCS
        "fonts/montserrat_ext_21.c"
        "fonts/montserrat_ext_24.c"
        "fonts/montserrat_ext_28.c"
        "fonts/montserrat_ext_31.c"
        "fonts/montserrat_ext_34.c")
else()
    set(BOARD_SRCS
        "boards/xiao_c6_display.c"
        "boards/xiao_c6_touch.c")
    set(FONT_SRCS
        "fonts/montserrat_ext_12.c"
        "fonts/montserrat_ext_14.c"
        "fonts/montserrat_ext_16.c"
        "fonts/montserrat_ext_18.c"
        "fonts/montserrat_ext_20.c")
endif()

idf_component_register(
    SRCS "main.c" "serial.c" "ui.c" ${BOARD_SRCS} ${FONT_SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_lcd nvs_flash json esp_timer
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
