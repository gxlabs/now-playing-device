idf_component_register(
    SRCS "main.c" "display.c" "touch.c" "serial.c" "ui.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_lcd nvs_flash json esp_timer
)
