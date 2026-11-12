idf_component_register(
    SRCS "eventloop.c" "console.c"
    INCLUDE_DIRS "../include"
    REQUIRES esp_event esp_timer freertos console driver vfs esp_driver_uart esp_driver_usb_serial_jtag cmd_system
)
