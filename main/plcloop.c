#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "argtable3/argtable3.h"
#include "driver/uart.h"

#include "eventloop.h"
#include "plc.h"
#include "board.h"

#include "main.h"

static const char* TAG = "plcloop";

/* Event ids */
#define PLCLOOP_RX_EVENT_ID         1   /* frame decoded (ISR) */
#define PLCLOOP_RX_ERROR_EVENT_ID   2   /* burst that was not a frame (ISR) */
#define PLCLOOP_TX_DONE_EVENT_ID    3   /* send finished (ISR) */
#define PLCLOOP_SEND_EVENT_ID       4   /* sc_plcloop_send */

/* Timer ids */
static const int PLCLOOP_GROUP_TIMER_ID = 1;
static const int PLCLOOP_STAT_TIMER_ID = 2;

/* Frames arrive a whole message at a time, when the message ends. An answer
 * then lands 60~95 ms after its poll, and the next poll ~127 ms or more after
 * that (docs/plc-protocol.md §2): 100 ms of silence closes a group. */
#define _GROUP_GAP_MS       100
#define _GROUP_MAX          48      /* a poll with a request and two answers is 17 */

/*
 * Message framing, inferred from the bus (docs/plc-protocol.md §5.2):
 *   header  high nibble = message type (bit 7 always set), low nibble =
 *           message length including header and checksum
 *   last    checksum = sum of the preceding bytes & 0x7F
 * Data bytes can have bit 7 set too, so only the length finds the next
 * header. A group is one MC200 poll with whatever answers follow it.
 */
#define _MSG_HEADER         0x80
#define _MSG_LEN_MASK       0x0F
#define _MSG_SUM_MASK       0x7F

/* The log: every complete group as a hex line on UART0 to the OpenLog.
 * Times are ms since boot — the board has no clock, and each power-up opens
 * a new LOGnnnnn.TXT, so a file starts at 0 after its '# boot' line. */
#define _DUMP_UART          UART_NUM_0
#define _DUMP_BAUD          115200
#define _DUMP_TX_BUF        4096
/* Nothing reads the OpenLog's replies while logging, so this keeps what it
 * printed since boot for the openlog command */
#define _DUMP_RX_BUF        1024

typedef struct _plc_send_data {
    uint8_t len;
    uint8_t data[SC_PLC_SEND_MAX];
} _plc_send_data_t;

typedef struct _plc_group {
    uint8_t data[_GROUP_MAX];
    int len;
} _plc_group_t;

typedef struct _plcloop {
    sc_eventloop_t loop;

    _plc_group_t cur;           /* group being received */
    _plc_group_t last;          /* last complete group */
    int64_t last_us;            /* when it completed */
    bool monitor;               /* log every frame */

    uint32_t frames;
    uint32_t groups;
    uint32_t group_changes;
    uint32_t overflows;         /* frames beyond _GROUP_MAX */
    uint32_t messages;
    uint32_t bad_messages;      /* wrong length, checksum or stray byte */
    uint32_t echoes;
    uint32_t err_no_start;
    uint32_t err_bits;
    uint32_t sent;
    _plc_send_data_t sending;   /* on the wire, joins the group when done */

    int64_t bus_us;             /* last traffic, ours or not */
    sc_rooms_t rooms;           /* what the messages say about each room */
} _plcloop_t;

static _plcloop_t *_plcloop;

/* Group as messages, e.g. "83 01 04 | E4 03 00 67". A message that fails
 * the framing rules is marked with '!'. Returns the number of bad messages. */
static int _group_str(const _plc_group_t *g, char *buf, size_t len, int *count)
{
    size_t n = 0;
    int bad = 0;
    int msgs = 0;
    int i = 0;

    buf[0] = '\0';
    while (i < g->len && n < len) {
        int mlen = 1;
        bool ok = false;

        if (g->data[i] & _MSG_HEADER) {
            mlen = g->data[i] & _MSG_LEN_MASK;
            if (mlen >= 2 && i + mlen <= g->len) {
                uint32_t sum = 0;
                for (int k = 0; k < mlen - 1; k++) sum += g->data[i + k];
                ok = (sum & _MSG_SUM_MASK) == g->data[i + mlen - 1];
            } else {
                mlen = g->len - i;
            }
        }

        n += snprintf(buf + n, len - n, "%s", msgs ? " | " : "");
        for (int k = 0; k < mlen && n < len; k++) {
            n += snprintf(buf + n, len - n, "%s%02X", k ? " " : "", g->data[i + k]);
        }
        if (!ok && n < len) {
            n += snprintf(buf + n, len - n, "!");
            bad++;
        }
        msgs++;
        i += mlen;
    }
    if (count) *count = msgs;
    return bad;
}

/* Feed every message that passes the framing rules to the room state, in
 * bus order. Same walk as _group_str. */
static void _group_feed(const _plc_group_t *g, int64_t now_us)
{
    int i = 0;

    while (i < g->len) {
        int mlen = 1;
        if (g->data[i] & _MSG_HEADER) {
            mlen = g->data[i] & _MSG_LEN_MASK;
            if (mlen >= 2 && i + mlen <= g->len) {
                uint32_t sum = 0;
                for (int k = 0; k < mlen - 1; k++) sum += g->data[i + k];
                if ((sum & _MSG_SUM_MASK) == g->data[i + mlen - 1]) {
                    sc_rooms_feed(&_plcloop->rooms, &g->data[i], mlen, now_us);
                }
            } else {
                mlen = g->len - i;
            }
        }
        i += mlen;
    }
}

/* ========== Driver callback — hand off to the plcloop thread ========== */

/**
 * PLC callback - runs in ISR context
 */
static void _plc_driver_cb(sc_plc_event_t event, int value, void *arg)
{
    if (!_plcloop || !_plcloop->loop) return;

    int id;
    switch (event) {
    case SC_PLC_EVENT_RX:       id = PLCLOOP_RX_EVENT_ID; break;
    case SC_PLC_EVENT_RX_ERROR: id = PLCLOOP_RX_ERROR_EVENT_ID; break;
    case SC_PLC_EVENT_TX_DONE:  id = PLCLOOP_TX_DONE_EVENT_ID; break;
    default: return;
    }
    sc_eventloop_post_event_from_isr(_plcloop->loop, id, &value, sizeof(value));
}

/* One "<ms> # ..." line to the log, for anything that is not a group */
static void _log_note(const char *fmt, ...)
{
    char line[160];
    int n = snprintf(line, sizeof(line), "%lld # ", esp_timer_get_time() / 1000);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 3) n = sizeof(line) - 3;
    n += snprintf(line + n, sizeof(line) - n, "\r\n");
    uart_write_bytes(_DUMP_UART, line, n);
}

/* ========== Event handlers — run in the plcloop thread ========== */

static void _stat_timer_cb(int id, void *arg, void *data)
{
    _log_note("stat frames %lu groups %lu bad %lu no-start %lu bits %lu overflow %lu echo %lu",
              _plcloop->frames, _plcloop->groups, _plcloop->bad_messages,
              _plcloop->err_no_start, _plcloop->err_bits, _plcloop->overflows, _plcloop->echoes);
}

static void _group_timer_cb(int id, void *arg, void *data)
{
    _plc_group_t *cur = &_plcloop->cur;
    if (cur->len == 0) return;

    _plcloop->groups++;
    _plcloop->last_us = esp_timer_get_time();

    char str[_GROUP_MAX * 5];
    int msgs = 0;
    int bad = _group_str(cur, str, sizeof(str), &msgs);
    _plcloop->messages += msgs;
    _plcloop->bad_messages += bad;

    /* Every group, with the time in ms, for logging on another machine */
    char line[sizeof(str) + 16];
    int n = snprintf(line, sizeof(line), "%lld %s\r\n", _plcloop->last_us / 1000, str);
    uart_write_bytes(_DUMP_UART, line, n);

    /* The bus repeats the same group ~4 times a second: log changes only */
    if (cur->len != _plcloop->last.len || memcmp(cur->data, _plcloop->last.data, cur->len)) {
        ESP_LOGI(TAG, "group %s", str);
        _plcloop->group_changes++;
    }

    _group_feed(cur, _plcloop->last_us);
    sc_displayloop_update(&_plcloop->rooms, _plcloop->bad_messages + _plcloop->err_no_start + _plcloop->err_bits);

    _plcloop->last = *cur;
    cur->len = 0;
}

static void _group_add(uint8_t byte)
{
    _plc_group_t *cur = &_plcloop->cur;
    if (cur->len < _GROUP_MAX) {
        cur->data[cur->len++] = byte;
    } else {
        _plcloop->overflows++;
    }
    sc_eventloop_add_timer(_plcloop->loop, PLCLOOP_GROUP_TIMER_ID, _group_timer_cb, NULL, _GROUP_GAP_MS, false);
}

/* Send from the loop thread. What we send joins the group once it is on
 * the bus, so the dump shows both sides in bus order; its echo is dropped. */
static esp_err_t _send(const uint8_t *data, size_t len)
{
    /* TODO: listen before talk — wait for a quiet bus between groups */
    esp_err_t ret = sc_plc_send(data, len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "send failed: %s", esp_err_to_name(ret));
        return ret;
    }
    _plcloop->sending.len = len;
    memcpy(_plcloop->sending.data, data, len);
    _plcloop->bus_us = esp_timer_get_time();
    return ESP_OK;
}


static void _rx_event_handler(int id, void *arg, void *data)
{
    int value = *(int *)data;
    uint8_t byte = value & 0xff;

    _plcloop->bus_us = esp_timer_get_time();
    if (value & SC_PLC_RX_ECHO) {
        _plcloop->echoes++;
        if (_plcloop->monitor) printf("plc: echo %02X\n", byte);
        return;
    }

    _plcloop->frames++;
    if (_plcloop->monitor) printf("plc: rx %02X\n", byte);

    _group_add(byte);
}

/* Raw pulses of a burst that did not decode, as one UART0 line:
 * "<ms> # no-start H901 L118 H71 ..." (H = carrier on, L = off, µs) */
static void _dump_error_burst(const char *what)
{
    /* A few frames' worth is enough to see what went wrong */
    static uint16_t burst[400];
    static char line[sizeof(burst) / sizeof(burst[0]) * 7 + 48];

    size_t n = sc_plc_get_error_burst(burst, sizeof(burst) / sizeof(burst[0]));
    int len = snprintf(line, sizeof(line), "%lld # %s", esp_timer_get_time() / 1000, what);
    for (size_t i = 0; i < n && len < (int)sizeof(line) - 12; i++) {
        len += snprintf(line + len, sizeof(line) - len, " %c%u",
                        burst[i] & SC_PLC_BURST_LEVEL ? 'H' : 'L', burst[i] & 0x7fff);
    }
    len += snprintf(line + len, sizeof(line) - len, "\r\n");
    uart_write_bytes(_DUMP_UART, line, len);
}

static void _rx_error_event_handler(int id, void *arg, void *data)
{
    int err = *(int *)data;

    _dump_error_burst(err == SC_PLC_RX_ERR_NO_START ? "no-start" : "bits");

    if (err == SC_PLC_RX_ERR_NO_START) {
        _plcloop->err_no_start++;
    } else {
        _plcloop->err_bits++;
    }
    if (_plcloop->monitor) printf("plc: rx error %s\n", err == SC_PLC_RX_ERR_NO_START ? "no-start" : "bits");
}

static void _tx_done_event_handler(int id, void *arg, void *data)
{
    _plcloop->sent++;
    _plcloop->bus_us = esp_timer_get_time();

    /* Added at the end, not the start: a send takes 12 ms a byte and the
     * answer comes 60~95 ms after it, so the group gap must count from here */
    for (int i = 0; i < _plcloop->sending.len; i++) _group_add(_plcloop->sending.data[i]);
    _plcloop->sending.len = 0;
    if (_plcloop->monitor) printf("plc: tx done\n");
}

static void _send_event_handler(int id, void *arg, void *data)
{
    _plc_send_data_t *send = (_plc_send_data_t *)data;

    _send(send->data, send->len);
}

/* ========== API ========== */

esp_err_t sc_plcloop_send(const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(_plcloop, ESP_ERR_INVALID_STATE, TAG, "plcloop not initialized");
    ESP_RETURN_ON_FALSE(data && len > 0 && len <= SC_PLC_SEND_MAX, ESP_ERR_INVALID_ARG, TAG, "invalid length");

    _plc_send_data_t send = { .len = len };
    memcpy(send.data, data, len);
    return sc_eventloop_post_event(_plcloop->loop, PLCLOOP_SEND_EVENT_ID, &send, sizeof(send));
}

/* ========== Console ========== */

/* The driver's free-size query is a conservative bound (ring buffer item
 * headers), so it never reads full when empty: ask whether TX is done instead */
static bool _dump_idle(void)
{
    return uart_wait_tx_done(_DUMP_UART, 0) == ESP_OK;
}

static int _plc_cmd(int argc, char **argv)
{
    char str[_GROUP_MAX * 5];
    _group_str(&_plcloop->last, str, sizeof(str), NULL);

    printf("last group : %s", _plcloop->last.len ? str : "-");
    if (_plcloop->last.len) {
        printf(" (%lld ms ago)", (esp_timer_get_time() - _plcloop->last_us) / 1000);
    }
    printf("\n");
    printf("frames     : %lu in %lu groups, %lu changes, %lu overflow\n",
           _plcloop->frames, _plcloop->groups, _plcloop->group_changes, _plcloop->overflows);
    printf("messages   : %lu, %lu bad (length / checksum)\n", _plcloop->messages, _plcloop->bad_messages);
    printf("errors     : %lu no-start, %lu bad bits\n", _plcloop->err_no_start, _plcloop->err_bits);
    printf("sent       : %lu sends, %lu echo frames\n", _plcloop->sent, _plcloop->echoes);
    printf("carrier    : %s\n", sc_plc_carrier() ? "on" : "off");
    printf("log        : UART0 %d baud, %s\n", _DUMP_BAUD, _dump_idle() ? "idle" : "sending");
    return 0;
}

static int _rooms_cmd(int argc, char **argv)
{
    int64_t now = esp_timer_get_time();
    char line[64];

    sc_rooms_boiler_line(&_plcloop->rooms, now, line, sizeof(line));
    printf("%s\n", line);
    for (int a = SC_ROOMS_ADDR_MIN; a <= SC_ROOMS_ADDR_MAX; a++) {
        const sc_room_t *r = &_plcloop->rooms.room[a];
        sc_rooms_line(&_plcloop->rooms, a, now, line, sizeof(line));
        printf("%-21s", line);
        if (r->seen_us) printf("  %lld s ago", (now - r->seen_us) / 1000000);
        if (r->registered) printf(", registered");
        if (r->timer_min > 0) printf(", timer %d min", r->timer_min);
        if (r->reserve_h > 0) printf(", reserve %d h", r->reserve_h);
        printf("\n");
    }
    return 0;
}

/* What the OpenLog printed on its TXO: "12<" is SD ready, file open, ready
 * for data; the same digits over and over mean it keeps resetting. Runs in
 * the console task, which may block; only this command reads UART0 RX. */
static struct { struct arg_int *secs; struct arg_end *end; } _openlog_args;

static int _openlog_cmd(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&_openlog_args) != 0) {
        arg_print_errors(stderr, _openlog_args.end, argv[0]);
        return 1;
    }
    int secs = _openlog_args.secs->count ? _openlog_args.secs->ival[0] : 0;
    int64_t end = esp_timer_get_time() + secs * 1000000LL;
    uint8_t buf[64];
    int total = 0;

    printf("openlog rx (hex / text), %d s:\n", secs);
    do {
        int n = uart_read_bytes(_DUMP_UART, buf, sizeof(buf), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            printf("%02X%c", buf[i], (total + i) % 16 == 15 ? '\n' : ' ');
        }
        if (n > 0) {
            printf(" | ");
            for (int i = 0; i < n; i++) putchar(buf[i] >= 0x20 && buf[i] < 0x7F ? buf[i] : '.');
            printf("\n");
            total += n;
        }
    } while (esp_timer_get_time() < end);
    printf("%d bytes\n", total);
    return 0;
}

static struct { struct arg_str *state; struct arg_end *end; } _plc_mon_args;

static int _plc_mon_cmd(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&_plc_mon_args) != 0) {
        arg_print_errors(stderr, _plc_mon_args.end, argv[0]);
        return 1;
    }
    _plcloop->monitor = strcmp(_plc_mon_args.state->sval[0], "on") == 0;
    printf("plc monitor %s\n", _plcloop->monitor ? "on" : "off");
    return 0;
}

static struct { struct arg_str *bytes; struct arg_end *end; } _plc_send_args;

static int _plc_send_cmd(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&_plc_send_args) != 0) {
        arg_print_errors(stderr, _plc_send_args.end, argv[0]);
        return 1;
    }

    uint8_t data[SC_PLC_SEND_MAX];
    int n = _plc_send_args.bytes->count;
    for (int i = 0; i < n; i++) {
        char *end;
        unsigned long v = strtoul(_plc_send_args.bytes->sval[i], &end, 16);
        if (*end || v > 0xff) {
            printf("invalid byte '%s' (hex 00~FF)\n", _plc_send_args.bytes->sval[i]);
            return 1;
        }
        data[i] = v;
    }
    return sc_plcloop_send(data, n) == ESP_OK ? 0 : 1;
}


static void _plcloop_cmd_init(void)
{

    ESP_ERROR_CHECK(esp_console_cmd_register(&(esp_console_cmd_t){
        .command = "plc", .help = "PLC bus status",
        .func = &_plc_cmd,
    }));

    _openlog_args.secs = arg_int0(NULL, NULL, "<s>", "keep listening this long, default 0");
    _openlog_args.end = arg_end(1);
    ESP_ERROR_CHECK(esp_console_cmd_register(&(esp_console_cmd_t){
        .command = "openlog", .help = "Show what the OpenLog sent back on UART0 RX (boot prompts)",
        .func = &_openlog_cmd, .argtable = &_openlog_args,
    }));

    ESP_ERROR_CHECK(esp_console_cmd_register(&(esp_console_cmd_t){
        .command = "rooms", .help = "Room state decoded from the bus (as on the OLED)",
        .func = &_rooms_cmd,
    }));

    _plc_mon_args.state = arg_str1(NULL, NULL, "<on|off>", "log every frame");
    _plc_mon_args.end = arg_end(1);
    ESP_ERROR_CHECK(esp_console_cmd_register(&(esp_console_cmd_t){
        .command = "plc_mon", .help = "Log every PLC frame",
        .func = &_plc_mon_cmd, .argtable = &_plc_mon_args,
    }));

    _plc_send_args.bytes = arg_strn(NULL, NULL, "<hex>", 1, SC_PLC_SEND_MAX, "bytes, one frame each");
    _plc_send_args.end = arg_end(SC_PLC_SEND_MAX);
    ESP_ERROR_CHECK(esp_console_cmd_register(&(esp_console_cmd_t){
        .command = "plc_send", .help = "Send PLC frames, e.g. plc_send 83 01 04",
        .func = &_plc_send_cmd, .argtable = &_plc_send_args,
    }));
}

/* ========== Init ========== */

static esp_err_t _dump_uart_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = _DUMP_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* TX buffer so a write never waits on the wire — a raw burst line can
     * run to a couple of kB */
    ESP_RETURN_ON_ERROR(uart_driver_install(_DUMP_UART, _DUMP_RX_BUF, _DUMP_TX_BUF, 0, NULL, 0), TAG, "uart install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(_DUMP_UART, &cfg), TAG, "uart config failed");
    return uart_set_pin(_DUMP_UART, BOARD_UART0_TX_PIN, BOARD_UART0_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

static void _plcloop_destroy(void)
{
    if (_plcloop) {
        if (_plcloop->loop) sc_eventloop_destroy(_plcloop->loop);
        free(_plcloop);
        _plcloop = NULL;
    }
}

esp_err_t sc_plcloop_init(void)
{
    esp_err_t ret = ESP_OK;

    if (_plcloop) {
        ESP_LOGW(TAG, "plcloop already initialized");
        return ESP_OK;
    }

    _plcloop = (_plcloop_t *)calloc(1, sizeof(_plcloop_t));
    ESP_RETURN_ON_FALSE(_plcloop, ESP_ERR_NO_MEM, TAG, "failed to allocate memory for plcloop");
    sc_rooms_init(&_plcloop->rooms);

    /* One receive can hold several frames, all posted from one ISR */
    _plcloop->loop = sc_eventloop_create("plcloop", 32, 4096);
    ESP_GOTO_ON_FALSE(_plcloop->loop, ESP_FAIL, _err, TAG, "failed to create plcloop");

    /* Register handlers before the driver callback can post */
    sc_eventloop_add_event(_plcloop->loop, PLCLOOP_RX_EVENT_ID, _rx_event_handler, NULL);
    sc_eventloop_add_event(_plcloop->loop, PLCLOOP_RX_ERROR_EVENT_ID, _rx_error_event_handler, NULL);
    sc_eventloop_add_event(_plcloop->loop, PLCLOOP_TX_DONE_EVENT_ID, _tx_done_event_handler, NULL);
    sc_eventloop_add_event(_plcloop->loop, PLCLOOP_SEND_EVENT_ID, _send_event_handler, NULL);

    ESP_GOTO_ON_ERROR(_dump_uart_init(), _err, TAG, "failed to init dump uart");
    _log_note("boot %s %s", SC_MODEL, SC_VERSION);

    if (CONFIG_SC_PLCLOG_STAT_S > 0) {
        sc_eventloop_add_timer(_plcloop->loop, PLCLOOP_STAT_TIMER_ID, _stat_timer_cb, NULL,
                               CONFIG_SC_PLCLOG_STAT_S * 1000, true);
    }

    sc_plc_register_callback(_plc_driver_cb, NULL);

    _plcloop_cmd_init();

    return ESP_OK;

_err:
    _plcloop_destroy();

    return ret;
}

void sc_plcloop_end(void)
{
    sc_plc_register_callback(NULL, NULL);
    _plcloop_destroy();
}
