#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "sdkconfig.h"
#include "board.h"
#include "oled.h"

#define _WIDTH          128
#define _HEIGHT         64
#define _CELL_W         6
#define _GLYPH_W        5
#define _MAX_SHIFT      (_WIDTH - SC_OLED_COLS * _CELL_W)
#define _I2C_HZ         (400 * 1000)

extern const uint8_t sc_font5x7[95][5];

static const char *TAG = "oled";

static i2c_master_bus_handle_t s_bus;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static bool s_ready;

/* SSD1306 page layout: 8 pages of 128 columns, one byte = 8 rows, LSB on top */
static uint8_t s_fb[_WIDTH * _HEIGHT / 8];
static int s_shift;

bool sc_oled_ready(void)
{
    return s_ready;
}

void sc_oled_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void sc_oled_set_shift(int px)
{
    s_shift = px < 0 ? 0 : px > _MAX_SHIFT ? _MAX_SHIFT : px;
}

void sc_oled_text(int row, int col, const char *str)
{
    if (row < 0 || row >= SC_OLED_ROWS) return;

    for (; *str && col < SC_OLED_COLS; str++, col++) {
        char c = *str;
        if (c < 0x20 || c > 0x7E) c = '?';
        uint8_t *dst = &s_fb[row * _WIDTH + s_shift + col * _CELL_W];
        memcpy(dst, sc_font5x7[c - 0x20], _GLYPH_W);
        dst[_GLYPH_W] = 0;
    }
}

esp_err_t sc_oled_flush(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, _WIDTH, _HEIGHT, s_fb);
}

esp_err_t sc_oled_on(bool on)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    return esp_lcd_panel_disp_on_off(s_panel, on);
}

esp_err_t sc_oled_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_OLED_SDA_PIN,
        .scl_io_num = BOARD_OLED_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* The modules carry their own 4.7~10 kΩ pull-ups; these only help */
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus failed");

    /* Probe first: without a panel the logger must still run */
    ESP_RETURN_ON_ERROR(i2c_master_probe(s_bus, BOARD_OLED_I2C_ADDR, 50), TAG,
                        "no panel at 0x%02X", BOARD_OLED_I2C_ADDR);

    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = BOARD_OLED_I2C_ADDR,
        .scl_speed_hz = _I2C_HZ,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,     /* SSD1306 control byte: Co, D/C# */
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_bus, &io_cfg, &s_io), TAG, "panel io failed");

    esp_lcd_panel_ssd1306_config_t ssd_cfg = { .height = _HEIGHT };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config = &ssd_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ssd1306(s_io, &panel_cfg, &s_panel), TAG, "panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    /* The driver leaves both mirrors off, which is upside down on the usual
     * 0.96" modules (header pins on top) */
#if CONFIG_SC_PLCLOG_OLED_FLIP
    bool mirror = false;
#else
    bool mirror = true;
#endif
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, mirror, mirror), TAG, "panel mirror failed");

    s_ready = true;
    sc_oled_clear();
    ESP_RETURN_ON_ERROR(sc_oled_flush(), TAG, "panel clear failed");
    ESP_RETURN_ON_ERROR(sc_oled_on(true), TAG, "panel on failed");

    ESP_LOGI(TAG, "OLED initialized (SDA=GPIO%d, SCL=GPIO%d)", BOARD_OLED_SDA_PIN, BOARD_OLED_SCL_PIN);
    return ESP_OK;
}
