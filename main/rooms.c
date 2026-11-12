#include <stdio.h>
#include <string.h>
#include "rooms.h"

/* Address byte (§5.4) */
#define _ADDR_MASK          0x0F
#define _ADDR_REGISTERED    0x40    /* MC200 poll */
#define _ADDR_DEMAND        0x20    /* from a room */
#define _ADDR_BOILER        0x10    /* from the MC200 */
#define _ADDR_E4_TEMP       0x10    /* E4: the value is the room temperature */

/* MC200 items start here; below are room items (§5.5) */
#define _MC200_ITEM_MIN     0x30
#define _MC200_HOT_WATER    0x3B
#define _READ_FULL_STATE    0x1E
#define _NO_READ            (-1)

void sc_rooms_init(sc_rooms_t *rooms)
{
    memset(rooms, 0, sizeof(*rooms));
    rooms->hot_water = SC_ROOM_UNKNOWN;
    for (int a = 0; a <= SC_ROOMS_ADDR_MAX; a++) {
        sc_room_t *r = &rooms->room[a];
        r->temp = r->setpoint = r->mode = r->timer_min = r->reserve_h = SC_ROOM_UNKNOWN;
        rooms->read_item[a] = _NO_READ;
    }
}

/* Notify / forward numbering (C5 from the room, A5 from the MC200) */
static void _set_item(sc_room_t *r, uint8_t item, uint8_t value, bool forward)
{
    switch (item) {
    case 0x00:
    case 0x08:
        /* A forwarded mode without 0x20 lifts "all off" back to that mode */
        if (forward && item == 0x08 && value < SC_ROOM_MODE_HEAT) value |= SC_ROOM_MODE_HEAT;
        r->mode = value;
        break;
    case 0x01: r->setpoint = value; break;
    case 0x03: r->timer_min = value; break;
    case 0x07: r->reserve_h = value; break;
    default: break;     /* 09, 0E, 17: not per-room state or unknown */
    }
}

/* Read numbering (answer to 94): values run on from the item read */
static void _set_read(sc_room_t *r, uint8_t item, const uint8_t *v, int n)
{
    if (item == _READ_FULL_STATE) {
        /* CA <addr> 1E <mode> <set> <room> <?> <away> <?> */
        if (n >= 1) r->mode = v[0];
        if (n >= 2) r->setpoint = v[1];
        if (n >= 3) r->temp = v[2];
        return;
    }
    for (int i = 0; i < n; i++) {
        switch (item + i) {
        case 0x00:
        case 0x02: r->mode = v[i]; break;
        case 0x03: r->setpoint = v[i]; break;
        case 0x05: r->timer_min = v[i]; break;
        default: break;     /* 04 timer mode, 06 away temperature */
        }
    }
}

void sc_rooms_feed(sc_rooms_t *rooms, const uint8_t *m, int len, int64_t now_us)
{
    if (len < 3) return;

    uint8_t type = m[0] & 0xF0;
    int addr = m[1] & _ADDR_MASK;
    sc_room_t *r = (addr >= SC_ROOMS_ADDR_MIN && addr <= SC_ROOMS_ADDR_MAX) ? &rooms->room[addr] : NULL;
    int nval = len - 4;     /* after header, address, item; before checksum */

    switch (type) {
    /* --- MC200 → rooms: all carry the boiler bit --- */
    case 0x80:  /* 83 <addr> poll */
    case 0x90:  /* 94 <addr> <item> read */
    case 0xB0:  /* B3 <addr> ack */
    case 0xA0:  /* A5 <addr> <item> <value> forward, or A5~A7 answer to D4 */
        rooms->mc200_us = now_us;
        rooms->boiler = m[1] & _ADDR_BOILER;
        if (!r) break;
        if (type == 0x80) {
            r->registered = m[1] & _ADDR_REGISTERED;
            rooms->read_item[addr] = _NO_READ;     /* a new poll ends any read */
        } else if (type == 0x90 && len >= 4) {
            rooms->read_item[addr] = m[2];
        } else if (type == 0xA0 && len >= 5) {
            if (m[2] == _MC200_HOT_WATER) {
                rooms->hot_water = m[3];
            } else if (m[2] < _MC200_ITEM_MIN) {
                _set_item(r, m[2], m[3], true);
            }
        }
        break;

    /* --- rooms → MC200: all carry the demand bit --- */
    case 0xE0:  /* E4 <addr|kind> <value> poll answer */
        if (!r) break;
        r->seen_us = now_us;
        r->demand = m[1] & _ADDR_DEMAND;
        if (m[1] & _ADDR_E4_TEMP) r->temp = m[2];
        break;
    case 0xC0:  /* Cn <addr> <item> <values...> notify or read answer */
        if (!r || nval < 1) break;
        r->seen_us = now_us;
        r->demand = m[1] & _ADDR_DEMAND;
        /* 1E is always the full state: it also answers the scan poll when a
         * room registers (§5.6), with no 94 before it */
        if (rooms->read_item[addr] == m[2] || m[2] == _READ_FULL_STATE) {
            _set_read(r, m[2], &m[3], nval);
            rooms->read_item[addr] = _NO_READ;
        } else {
            _set_item(r, m[2], m[3], false);
        }
        break;

    default:    /* D4 / F5 from the DT-200: the MC200's answer carries it */
        break;
    }
}

static const char *_mode_name(int mode, char *buf)
{
    switch (mode) {
    case SC_ROOM_UNKNOWN:       return "--";
    case SC_ROOM_MODE_HEAT:     return "HEAT";
    case SC_ROOM_MODE_RESERVE:  return "RSV";
    case SC_ROOM_MODE_OFF:      return "OFF";
    case SC_ROOM_MODE_AWAY:     return "AWAY";
    case SC_ROOM_MODE_TIMER:    return "TMR";
    default:
        sprintf(buf, "x%02X", mode & 0xFF);
        return buf;
    }
}

static const char *_temp(int16_t half, char *buf)
{
    if (half == SC_ROOM_UNKNOWN) return "--";
    sprintf(buf, "%d.%d", half / 2, (half & 1) * 5);
    return buf;
}

void sc_rooms_line(const sc_rooms_t *rooms, int addr, int64_t now_us, char *buf, int len)
{
    const sc_room_t *r = &rooms->room[addr];
    char name[16];
    char t[8], s[8], m[8];

    if (addr == SC_ROOMS_ADDR_MIN) {
        strcpy(name, "LV");
    } else {
        snprintf(name, sizeof(name), "R%d", addr - 1);
    }
    if (!r->seen_us && !r->registered) {
        snprintf(buf, len, "%s -", name);
        return;
    }
    bool stale = !r->seen_us || now_us - r->seen_us > SC_ROOM_STALE_US;
    snprintf(buf, len, "%-2s %4s/%-4s %-4s %c%c", name, _temp(r->temp, t), _temp(r->setpoint, s),
             _mode_name(r->mode, m), r->demand ? '*' : ' ', stale ? '?' : ' ');
}

void sc_rooms_boiler_line(const sc_rooms_t *rooms, int64_t now_us, char *buf, int len)
{
    char hw[8];

    if (!rooms->mc200_us || now_us - rooms->mc200_us > SC_ROOM_STALE_US) {
        snprintf(buf, len, "NO MC200");
        return;
    }
    snprintf(buf, len, "BOILER %-3s HW %s", rooms->boiler ? "ON" : "off", _temp(rooms->hot_water, hw));
}
