idf_component_register(
    SRCS "plcloop.c" "displayloop.c" "rooms.c" "mainloop.c" "main.c"
    INCLUDE_DIRS "."
    REQUIRES core plc oled esp_timer esp_driver_uart
)
