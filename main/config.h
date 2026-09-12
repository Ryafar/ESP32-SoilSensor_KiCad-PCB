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

/* Which of the two panels is fitted is a build-time choice and lives in
 * components/eink/include/eink_panel.h - it decides which controller driver is
 * compiled, so it cannot come from here. */

/* SPI2 is the only general-purpose SPI host on the C6, and the display is the
 * only device on it. Expanded at the point of use, so config.h stays free of
 * ESP-IDF headers. */
#define EPD_SPI_HOST        SPI2_HOST

/* AO3401A is a P-FET high-side switch: the gate goes to GND to conduct.
 * Confirmed against the schematic. */
#define EPD_POWER_ACTIVE_LOW 1

/* --- Soil probe (CON2) --------------------------------------------------- */
#define PIN_SOIL_MEAS       1   // ADC input
#define PIN_SOIL_POWER     14   // load switch, power the probe only while sampling

/* Same AO3401A P-FET arrangement as the display rail. */
#define SOIL_POWER_ACTIVE_LOW 1

/* Time between switching the probe rail on and the first conversion. The
 * hardware document only ever guessed at "tens of ms"; soil_test_task samples a
 * ramp after power-up and logs where the output actually flattens, so this is
 * the number to correct once that ramp has been read off a real probe.
 * Must stay above one FreeRTOS tick (10 ms at CONFIG_FREERTOS_HZ=100) -
 * pdMS_TO_TICKS truncates anything shorter to no delay at all. */
#define SOIL_SETTLE_MS              100
#define SOIL_ADC_SAMPLES            16      // averaged per reading

/* Two-point calibration, docs/product-design.md §7. Dry was measured during
 * bring-up by holding the probe in air.
 *
 * Wet is NOT measured yet - this is a placeholder so the percentage has a scale
 * at all, and every percentage the firmware reports is wrong by whatever the
 * real wet point turns out to be. The millivolts next to it in the log are the
 * number to trust until the probe has been in water at working depth. */
#define SOIL_CALIBRATION_DRY_MV     2719
#define SOIL_CALIBRATION_WET_MV     1200    // PROVISIONAL, see above

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

/* CHARGE_EN is the charger's /CE pin: low (or floating) enables charging while
 * VIN is valid, high disables it, and it has no effect at all without VIN.
 *
 * The chip's internal 5 MOhm pulldown means an unconnected pin would enable
 * charging - but this board's 10 kOhm pull-up overrides that by three orders of
 * magnitude, so on this board charging is DISABLED until firmware drives the
 * pin low. That is why nothing charged before this was implemented.
 *
 * Driving it low costs VSYS/10k, about 330 uA, for as long as it is low. That
 * is irrelevant next to a 250 mA charge current and 33x the whole deep-sleep
 * budget, so the pin goes low only while VIN is actually present.
 *
 * A future board revision should drop this pull-up (or make it a pull-down):
 * the chip's own default is the state you want, and then charging would work
 * even with no firmware running at all. */
#define CHARGE_EN_ACTIVE_LOW 1

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
/* The 10 mA reset default must be overwritten, and the charger watchdog reverts
 * every register after 160 s without an I2C transaction - so this is rewritten
 * on every wake rather than once at first boot (product design §3.3). */
#define BQ25186_REG_ICHG_CTRL       0x04
#define BQ25186_ICHG_500MA          0x4D    // raw register code, bring-up pokes only

/* The cell is the knob; the current follows from it. 0.5C is safe for this
 * 500 mAh pack without leaning on its datasheet allowing 1C, and it stays safe
 * if a larger cell is fitted instead (product design §3.4, which supersedes the
 * 500 mA figure in hardware README §4.2). */
#define BATTERY_CAPACITY_MAH        500
#define BATTERY_CHARGE_RATE_PERCENT 50
#define BATTERY_CHARGE_CURRENT_MA   (BATTERY_CAPACITY_MAH * BATTERY_CHARGE_RATE_PERCENT / 100)

/* --- Battery measurement -------------------------------------------------- */
/* 1:2 divider, 2 x 100 kOhm with 100 nF to GND. Kept as a number rather than a
 * literal *2 in the code: resistor tolerance means the real ratio is not
 * exactly 2, so calibrating against a multimeter changes this one constant.
 * At 4.2 V the ADC sees 2.1 V, which fits ADC_ATTEN_DB_12. */
#define BATTERY_DIVIDER_RATIO       2.0f
/* With BAT_MEAS_EN low the BSS138 opens the divider's low leg, so the ADC node
 * is pulled up to VBAT through the top 100k and sits there. Every reading
 * therefore starts by discharging the 100 nF through 100k||100k: tau = 5 ms,
 * and it takes about 5 tau to land within a millivolt of VBAT/2.
 * Must also stay above one FreeRTOS tick (10 ms at CONFIG_FREERTOS_HZ=100) -
 * pdMS_TO_TICKS truncates, and anything below a tick does not delay at all. */
#define BATTERY_SETTLE_MS           30
#define BATTERY_ADC_SAMPLES         16      // averaged per reading

/* --- Battery thresholds -------------------------------------------------- */
/* Soft threshold, enforced in firmware: below this the board enters ship mode.
 * The charger's own BUVLO sits at 3.0 V and is only a backstop. */
#define BATTERY_SHIP_MODE_MV        3300
/* Leaving the low state happens higher than entering it. A cell at 3.35 V sags
 * below 3.3 V the moment WiFi transmits, and a single threshold would produce a
 * device that shuts down, gets plugged in, boots and shuts down again
 * (product design §3.2). */
#define BATTERY_RECOVER_MV          3500

/* --- Timing -------------------------------------------------------------- */
/* The soil probe's settle time lives with the other soil constants above.
 *
 * Two separate numbers on purpose: the board wakes to measure, and only every
 * n-th measurement pays for the radio. 5 min x 6 = one publish every half hour
 * is the design target (product design §2).
 *
 * Both are turned down for bench work, where a half hour between log lines just
 * means waiting. The design values are in the comment, not lost. */
#define MEASUREMENT_INTERVAL_S      60      // design: 5 * 60
#define PUBLISH_EVERY_N_MEASUREMENTS 1      // design: 6
