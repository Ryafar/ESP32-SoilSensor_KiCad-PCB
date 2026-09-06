/* Board configuration for the Soil Sensor, KiCad project rev 0.1.0.
 *
 * Single source of truth for pin assignment and system-wide constants.
 * Taken from docs/hardware/README.md - keep both in sync when the board
 * changes. Drivers under components/ stay board-agnostic and receive these
 * values through their init structs.
 */
#pragma once

/* --- E-ink display, SPI (CON1) ------------------------------------------ */
#define PIN_EPD_BUSY        2   // input
#define PIN_EPD_RST         3   // output
#define PIN_EPD_DC          4   // output
#define PIN_EPD_CS          5   // output
#define PIN_EPD_CLK         6   // SPI SCLK
#define PIN_EPD_MOSI        7   // SPI MOSI, display is write-only
#define PIN_EPD_POWER      23   // load switch for the display rail (AO3401A)

/* SPI2 is the only general-purpose SPI host on the C6, and the display is the
 * only device on it. Expanded at the point of use, so config.h stays free of
 * ESP-IDF headers. */
#define EPD_SPI_HOST        SPI2_HOST

/* AO3401A is a P-FET high-side switch: the gate goes to GND to conduct.
 * Confirmed against the schematic. */
#define EPD_POWER_ACTIVE_LOW 1

/* --- Soil probe (CON2) --------------------------------------------------- */
#define PIN_SOIL_MEAS       1   // ADC input
#define PIN_SOIL_POWER     15   // load switch, power the probe only while sampling

/* Same AO3401A P-FET arrangement as the display rail. */
#define SOIL_POWER_ACTIVE_LOW 1

/* --- Battery / charger BQ25186 ------------------------------------------- */
#define PIN_I2C_SDA        18
#define PIN_I2C_SCL        19
#define PIN_CHARGER_INT    20   // input, active low, 10k pull-up on the board
#define PIN_CHARGE_EN      21   // output, 10k pull-up on the board
#define PIN_POWER_GOOD     22   // input, 10k pull-up on the board
#define PIN_BAT_MEAS        0   // ADC input, behind the divider
#define PIN_BAT_MEAS_EN    16   // BSS138, assert only while reading

/* The charger pins carry 10 kOhm pull-ups, so the ESP32's internal ones are
 * redundant on INT, CHARGE_EN and POWER_GOOD. BAT_MEAS_EN has a pull-down
 * instead: the divider stays disconnected while the MCU sleeps or is in reset,
 * and only a driven high connects it. */
#define BAT_MEAS_EN_ACTIVE_HIGH 1

/* --- RGB status LED ------------------------------------------------------ */
/* Wuerth WL-ICLED 1312020030000, WS2812-compatible single-wire protocol.
 * Strapping pin: must not be held low at reset. */
#define PIN_RGB_DIN         8

/* --- Buttons ------------------------------------------------------------- */
#define PIN_BOOT_BUTTON     9   // strapping pin, low at reset -> download mode

/* --- I2C ----------------------------------------------------------------- */
#define I2C_CLK_HZ          100000  // 100 kHz while bringing the bus up
#define BQ25186_I2C_ADDR    0x6A

/* --- Charger configuration ----------------------------------------------- */
/* Register 0x04: CHG_DIS = 0, ICHG = 500 mA (code 77). The 10 mA reset default
 * must be overwritten. The charger watchdog reverts registers after 160 s, so
 * this gets rewritten on every wake rather than once at first boot. */
#define BQ25186_REG_ICHG_CTRL       0x04
#define BQ25186_ICHG_500MA          0x4D

/* --- Battery measurement -------------------------------------------------- */
/* 1:2 divider, 2 x 100 kOhm with 100 nF to GND. Kept as a number rather than a
 * literal *2 in the code: resistor tolerance means the real ratio is not
 * exactly 2, so calibrating against a multimeter changes this one constant.
 * At 4.2 V the ADC sees 2.1 V, which fits ADC_ATTEN_DB_12. */
#define BATTERY_DIVIDER_RATIO       2.0f
#define BATTERY_SETTLE_MS           5       // let the 100 nF recharge after enabling
#define BATTERY_ADC_SAMPLES         16      // averaged per reading

/* --- Battery thresholds -------------------------------------------------- */
/* Soft threshold, enforced in firmware: below this the board enters ship mode.
 * The charger's own BUVLO sits at 3.0 V and is only a backstop. */
#define BATTERY_SHIP_MODE_MV        3300

/* --- Timing -------------------------------------------------------------- */
#define SOIL_PROBE_SETTLE_MS        50      // after asserting SOIL_POWER
#define MEASUREMENT_INTERVAL_S      (30 * 60)
