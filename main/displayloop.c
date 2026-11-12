#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_timer.h"

#include "eventloop.h"
#include "oled.h"

#include "main.h"

static const char* TAG = "displayloop";

/* Event ids */
#define DISPLAYLOOP_UPDATE_EVENT_ID     1   /* sc_displayloop_update */

/* Timer ids */
static const int DISPLAYLOOP_DRAW_TIMER_ID = 1;
static const int DISPLAYLOOP_SHIFT_TIMER_ID = 2;

/* Once a second is enough for values that change a few times a minute, and
 * it also ages rooms into '?' when they stop answering */
#define _DRAW_MS            1000
/* Burn-in: the text walks 0→1→2 px right, one step a minute */
#define _SHIFT_MS           (60 * 1000)
#define _SHIFT_STEPS        3

typedef struct _display_data {
    sc_rooms_t rooms;
    uint32_t errors;
} _display_data_t;

typedef struct _displayloop {
    sc_eventloop_t loop;
    _display_data_t data;
    int shift;
} _displayloop_t;

static _displayloop_t *_displayloop;

/* ========== Event handlers — run in the displayloop thread ========== */

static void _update_event_handler(int id, void *arg, void *data)
{
    _displayloop->data = *(_display_data_t *)data;
}

static void _draw_timer_cb(int id, void *arg, void *data)
{
    const _display_data_t *d = &_displayloop->data;
    int64_t now = esp_timer_get_time();
    char line[SC_OLED_COLS + 8];

    sc_oled_clear();
    sc_oled_set_shift(_displayloop->shift);

    sc_rooms_boiler_line(&d->rooms, now, line, sizeof(line));
    sc_oled_text(0, 0, line);
    for (int a = SC_ROOMS_ADDR_MIN; a <= SC_ROOMS_ADDR_MAX; a++) {
        sc_rooms_line(&d->rooms, a, now, line, sizeof(line));
        sc_oled_text(a, 0, line);
    }
    int64_t up_s = now / 1000000;
    snprintf(line, sizeof(line), "up %lld:%02lld:%02lld err %lu", up_s / 3600, up_s / 60 % 60, up_s % 60, d->errors);
    sc_oled_text(SC_OLED_ROWS - 1, 0, line);

    esp_err_t ret = sc_oled_flush();
    if (ret != ESP_OK) ESP_LOGW(TAG, "flush failed: %s", esp_err_to_name(ret));
}

static void _shift_timer_cb(int id, void *arg, void *data)
{
    _displayloop->shift = (_displayloop->shift + 1) % _SHIFT_STEPS;
}

/* ========== API ========== */

esp_err_t sc_displayloop_update(const sc_rooms_t *rooms, uint32_t errors)
{
    if (!_displayloop) return ESP_ERR_INVALID_STATE;

    _display_data_t d = { .rooms = *rooms, .errors = errors };
    return sc_eventloop_post_event(_displayloop->loop, DISPLAYLOOP_UPDATE_EVENT_ID, &d, sizeof(d));
}

/* ========== Init ========== */

esp_err_t sc_displayloop_init(void)
{
    if (_displayloop) {
        ESP_LOGW(TAG, "displayloop already initialized");
        return ESP_OK;
    }
    /* No panel: logging goes on without a screen */
    if (!sc_oled_ready()) {
        ESP_LOGW(TAG, "no OLED, display off");
        return ESP_OK;
    }

    _displayloop = (_displayloop_t *)calloc(1, sizeof(_displayloop_t));
    ESP_RETURN_ON_FALSE(_displayloop, ESP_ERR_NO_MEM, TAG, "failed to allocate memory for displayloop");
    sc_rooms_init(&_displayloop->data.rooms);

    _displayloop->loop = sc_eventloop_create("displayloop", 8, 4096);
    if (!_displayloop->loop) {
        free(_displayloop);
        _displayloop = NULL;
        ESP_LOGE(TAG, "failed to create displayloop");
        return ESP_FAIL;
    }

    sc_eventloop_add_event(_displayloop->loop, DISPLAYLOOP_UPDATE_EVENT_ID, _update_event_handler, NULL);
    sc_eventloop_add_timer(_displayloop->loop, DISPLAYLOOP_DRAW_TIMER_ID, _draw_timer_cb, NULL, _DRAW_MS, true);
    sc_eventloop_add_timer(_displayloop->loop, DISPLAYLOOP_SHIFT_TIMER_ID, _shift_timer_cb, NULL, _SHIFT_MS, true);
    return ESP_OK;
}
