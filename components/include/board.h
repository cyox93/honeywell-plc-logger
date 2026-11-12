#pragma once

/*
 * plc-log board: ESP32-C3 Super Mini + dt200-plc-module + OpenLog.
 *
 * The only place GPIO numbers are defined. Mirrored in README.md — change
 * both together.
 *
 * GPIO 2/8/9 are strapping pins (8 is also the on-board LED, 9 the BOOT
 * button), GPIO 18/19 are the native USB used for the console.
 */

/* --- PLC module (dt200-plc-module J4) ---------------------------------- */

/* /SCTL: LOW = send carrier. The module pulls it up to +5 V through 10 kΩ,
 * so the pin is open-drain only — drive LOW, release for HIGH. */
#define BOARD_PLC_SCTL_PIN      5
/* DOUT_BUF: HIGH = carrier on the bus. 3.3 V level (module R23 20 kΩ). */
#define BOARD_PLC_DOUT_PIN      6

/* --- OpenLog on UART0 --------------------------------------------------- */

/* The C3's default UART0 pins. The ROM still prints its boot banner here, so
 * every reset leaves a few lines of ROM text in the log. */
#define BOARD_UART0_TX_PIN      21      /* → OpenLog RXI */
#define BOARD_UART0_RX_PIN      20      /* ← OpenLog TXO, not read */

/* --- OLED (0.96" SSD1306 128×64, I2C) ----------------------------------- */

/* The SDA/SCL marked on the Super Mini pinout. Both are strapping pins, but
 * the module's pull-ups hold them HIGH at reset, which is a normal boot. The
 * on-board LED on GPIO8 flickers with the I2C traffic. */
#define BOARD_OLED_SDA_PIN      8
#define BOARD_OLED_SCL_PIN      9
#define BOARD_OLED_I2C_ADDR     0x3C
