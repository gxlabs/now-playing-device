idf_component_register(
    SRCS "main.c" "display.c" "touch.c" "serial.c" "ui.c"
         "fonts/montserrat_ext_12.c"
         "fonts/montserrat_ext_14.c"
         "fonts/montserrat_ext_16.c"
         "fonts/montserrat_ext_18.c"
         "fonts/montserrat_ext_20.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_lcd nvs_flash json esp_timer
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
