#pragma once

/*
 * Per-room state picked out of the bus messages (docs/plc-protocol.md §5).
 * Pure bookkeeping — the owner (plcloop) feeds every message that passed the
 * length and checksum check, in bus order.
 *
 * Temperatures are raw bus values in 0.5 °C units. Only what the bus says is
 * known: set point and mode appear when a room notifies a change or answers
 * a full-state read, so right after power-up only the room temperature fills.
 */

#include <stdbool.h>
#include <stdint.h>

/* Address 1 = living room (DT-200), 2~6 = room 1~5 */
#define SC_ROOMS_ADDR_MIN   1
#define SC_ROOMS_ADDR_MAX   6
#define SC_ROOM_UNKNOWN     (-1)

/* A room that has not answered for this long is shown stale; the MC200
 * drops it from polling after ~9 s (§5.6) */
#define SC_ROOM_STALE_US    (10 * 1000 * 1000)

/* Mode values (§5.5) */
#define SC_ROOM_MODE_HEAT       0x20
#define SC_ROOM_MODE_RESERVE    0x28
#define SC_ROOM_MODE_OFF        0x2A
#define SC_ROOM_MODE_AWAY       0x2B
#define SC_ROOM_MODE_TIMER      0x40

typedef struct sc_room {
    int64_t seen_us;        /* last message from the room, 0 = never */
    bool registered;        /* MC200 polls it with 0x40 */
    bool demand;            /* heating demand, 0x20 */
    int16_t temp;           /* room temperature, 0.5 °C */
    int16_t setpoint;       /* 0.5 °C */
    int16_t mode;           /* SC_ROOM_MODE_* or another raw value */
    int16_t timer_min;
    int16_t reserve_h;
} sc_room_t;

typedef struct sc_rooms {
    sc_room_t room[SC_ROOMS_ADDR_MAX + 1];  /* by address */
    int64_t mc200_us;       /* last MC200 message, 0 = never */
    bool boiler;            /* boiler running, 0x10 on MC200 messages */
    int16_t hot_water;      /* 0.5 °C, read by the DT-200 */
    /* Item of the MC200's pending 94 read per address: the answer uses the
     * read numbering, not the notify numbering */
    int16_t read_item[SC_ROOMS_ADDR_MAX + 1];
} sc_rooms_t;

void sc_rooms_init(sc_rooms_t *rooms);
/* One whole message: header ... checksum */
void sc_rooms_feed(sc_rooms_t *rooms, const uint8_t *msg, int len, int64_t now_us);

/* "LV 23.5/24.0 HEAT *?" — 20 chars; '*' = demand, '?' = stale */
void sc_rooms_line(const sc_rooms_t *rooms, int addr, int64_t now_us, char *buf, int len);
/* "BOILER ON  HW 58.0" */
void sc_rooms_boiler_line(const sc_rooms_t *rooms, int64_t now_us, char *buf, int len);
