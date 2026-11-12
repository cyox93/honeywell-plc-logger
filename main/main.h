#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "plc.h"
#include "rooms.h"

#define SC_MODEL    "PLC-LOG"
#define SC_VERSION  "0.1.0"

esp_err_t sc_mainloop_init(void);

/*
 * plcloop — owner of the PLC bus link and the OpenLog. Frames are gathered
 * into groups (frames 12 ms apart, groups 254 ms apart) and every group is
 * written to the OpenLog as one line (docs/plc-protocol.md §6.1). Messages
 * that pass the checksum also update the room state for the display.
 */
esp_err_t sc_plcloop_init(void);
void sc_plcloop_end(void);
/* Queues one frame per byte (≤ SC_PLC_SEND_MAX); the plcloop thread sends them */
esp_err_t sc_plcloop_send(const uint8_t *data, size_t len);

/*
 * displayloop — owner of the OLED. Draws the latest room state once a second
 * in its own thread, so the I2C transfer never holds up plcloop. Does nothing
 * when no panel answered at boot.
 */
esp_err_t sc_displayloop_init(void);
/* Posts a copy; the next draw shows it */
esp_err_t sc_displayloop_update(const sc_rooms_t *rooms, uint32_t errors);
