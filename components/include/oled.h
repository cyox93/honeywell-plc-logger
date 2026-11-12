#pragma once

/*
 * 0.96" SSD1306 128×64 OLED on I2C, as a 21 × 8 text screen (5×7 font in
 * 6×8 cells, ASCII only). Draw into the frame buffer, then flush — only
 * flush touches the bus (~25 ms at 400 kHz).
 */

#include <stdbool.h>
#include "esp_err.h"

#define SC_OLED_COLS    21
#define SC_OLED_ROWS    8

/* Fails when no panel answers; everything else is then a no-op */
esp_err_t sc_oled_init(void);
bool sc_oled_ready(void);

void sc_oled_clear(void);
/* Text from (row, col); cut at the right edge */
void sc_oled_text(int row, int col, const char *str);
/* Moves everything drawn afterwards right by 0~2 px, so fixed text does not
 * burn into the same pixels */
void sc_oled_set_shift(int px);
esp_err_t sc_oled_flush(void);
esp_err_t sc_oled_on(bool on);
