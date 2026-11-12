# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

GitHub repo `honeywell-plc-logger` — a Honeywell thermostat PLC-protocol logger.
ESP-IDF v6.0 firmware (project name `plc-log`, kept as-is) for an **ESP32-C3 Super Mini**
that sniffs the Honeywell boiler-bus (MC200 / DT-200 / LT200SPD) PLC traffic, logs it to
an **OpenLog** (microSD) on UART0 and shows each room's state on a 0.96" SSD1306 I2C OLED.
It is the PLC part of the sibling repository `../x3-thermostat` (ESP32-S3 main-board) cut down to a logger: the RMT driver
`components/plc/` and the core `eventloop` / `console` are copied from there unchanged
except for C3 comments. If the driver changes in one repo, carry it to the other.

`docs/plc-protocol.md` is a copy from `x3-thermostat` — the waveform, frame and message
rules — with a header note and C3 pin numbers added (§1, §6.1); §6–§8 still describe the
S3 firmware. Keep those local edits when re-copying it. `README.md` has the wiring, OpenLog config and the log line format.

## Build

```sh
source ~/.espressif/tools/activate_idf_v6.0.sh
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

In non-interactive shells `idf.py` may be only an alias; call it as
`"$HOME/.espressif/tools/python/v6.0/venv/bin/python" "$IDF_PATH/tools/idf.py" ...`.
Config lives in `sdkconfig.defaults` and `main/Kconfig.projbuild`; `sdkconfig` is generated.
There are no tests — back behavioural claims with evidence from the bench.

## Architecture

- **`components/include/board.h`** — the only place GPIO numbers are defined; keep it in
  sync with the table in `README.md`.
- **`components/plc/`** — RMT RX decodes one frame = one byte in the ISR; TX is
  open-drain on `/SCTL`. The C3 has 4 RMT blocks: TX takes 1, RX 2 (ping-pong).
- **`main/plcloop.c`** — one `sc_eventloop`: gathers frames into groups (100 ms gap),
  splits messages, writes one line per group to the OpenLog, `# ...` lines for boot,
  stat and raw error bursts. Owns the room table: every message that passes the
  checksum goes to `sc_rooms_feed`, and a copy is posted to displayloop per group.
  Callbacks only post; state changes in the loop thread.
- **`main/rooms.c`** — message → per-room state (address, demand/boiler bits, items in
  notify vs. read numbering). No state of its own; the caller owns `sc_rooms_t`.
  Protocol meanings live in `docs/plc-protocol.md` §5 — change both together.
- **`main/displayloop.c`** — owns the OLED; redraws once a second in its own thread so the
  ~25 ms I2C flush never delays plcloop. Absent panel = no display, logging unaffected.
- **`components/oled/`** — SSD1306 via `esp_lcd`, 21×8 text frame buffer, built-in 5×7 font.

## Hardware traps

1. **`/SCTL` (GPIO5) must be open-drain** — the module pulls it to +5 V.
2. **UART0 (GPIO20/21) is the OpenLog.** The console is USB Serial/JTAG; never move
   logging or the console back onto UART0. The ROM boot banner still lands in the log.
3. GPIO 2/8/9 are strapping pins (8 = on-board LED, 9 = BOOT). 8/9 carry the OLED I2C
   (the pinout's SDA/SCL); the module's pull-ups keep them HIGH at reset. Nothing on
   them may pull LOW at reset, and GPIO2 stays free.

## Conventions

Same as `x3-thermostat`: `sc_` prefix on public symbols, comments explain *why*, docs in
Korean, commit messages `{type} : {scope} : {sub-scope} : {summary}` with a body indented
2 spaces and the Co-Authored-By trailer.
