#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"

#include "console.h"
#include "plc.h"
#include "oled.h"
#include "main.h"

void app_main(void)
{
    printf("\n\n%s %s\n\n", SC_MODEL, SC_VERSION);

    ESP_ERROR_CHECK(sc_console_init());
    ESP_ERROR_CHECK(sc_plc_init());
    /* The screen is optional; the log is what matters */
    if (sc_oled_init() != ESP_OK) printf("OLED not found, running without a screen\n");

    ESP_ERROR_CHECK(sc_mainloop_init());
}
