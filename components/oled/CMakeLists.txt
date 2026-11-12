idf_component_register(
    SRCS "oled.c" "font5x7.c"
    INCLUDE_DIRS "../include"
    REQUIRES esp_lcd esp_driver_i2c
)
