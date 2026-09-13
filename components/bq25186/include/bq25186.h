/* TI BQ25186 - 1-cell 1 A linear battery charger with power path and ship mode.
 *
 * Chip driver: register access, charge current, status, ship mode. No policy -
 * when to charge, which voltage counts as "low" and when the board should shut
 * itself down is the application's decision, not this driver's.
 *
 * The I2C bus is borrowed. The caller creates the master bus and passes the
 * handle in; this driver only adds itself as a device on it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BQ25186_I2C_ADDR_DEFAULT 0x6A

/* Register map. Addresses are from the datasheet's register summary. The reset
 * values of 0x03, 0x04, 0x07 and 0x09 have been confirmed against a real board
 * (see docs/hardware/README.md §4.1a); the individual status bits in STAT0 and
 * STAT1 have not, which is why bq25186_read_status() hands back raw bytes. */
#define BQ25186_REG_STAT0        0x00 // charging state
#define BQ25186_REG_STAT1        0x01 // input/battery/temperature state
#define BQ25186_REG_FLAG0        0x02 // latched fault flags, cleared on read
#define BQ25186_REG_VBAT_CTRL    0x03 // charge voltage, reset 0x46 = 4.2 V
#define BQ25186_REG_ICHG_CTRL    0x04 // charge disable + charge current
#define BQ25186_REG_CHARGECTRL0  0x05 // precharge, termination, thermal regulation
#define BQ25186_REG_CHARGECTRL1  0x06 // BUVLO, discharge OCP, interrupt masks
#define BQ25186_REG_IC_CTRL      0x07 // safety timer, watchdog
#define BQ25186_REG_TMR_ILIM     0x08 // input current limit, autowake
#define BQ25186_REG_SHIP_RST     0x09 // ship mode, register reset, push button
#define BQ25186_REG_SYS_REG      0x0A // system regulation voltage
#define BQ25186_REG_TS_CONTROL   0x0B // thermistor thresholds
#define BQ25186_REG_MASK_ID      0x0C // interrupt masks and device ID

#define BQ25186_REG_FIRST        BQ25186_REG_STAT0
#define BQ25186_REG_LAST         BQ25186_REG_MASK_ID

/* ICHG_CTRL (0x04) */
#define BQ25186_ICHG_CTRL_CHG_DIS_MASK 0x80 // 1 = charging disabled
#define BQ25186_ICHG_CTRL_ICHG_MASK    0x7F

/* STAT0 (0x00). Bit positions from docs/hardware/README.md §4.4, which is the
 * datasheet §6.5.1.1 read against a real dump. Only the two fields the
 * application needs are named; the rest stay raw in bq25186_status_t until
 * something actually reads them. */
#define BQ25186_STAT0_CHG_STAT_MASK    0x60 // bits 6:5, see bq25186_charge_state_t
#define BQ25186_STAT0_CHG_STAT_SHIFT   5
#define BQ25186_STAT0_VIN_PGOOD_MASK   0x01 // 1 = VIN above UVLO and VBAT+V(SLEEP)

/* SHIP_RST (0x09): EN_RST_SHIP occupies bits [6:5], 0b10 enters ship mode. */
#define BQ25186_SHIP_RST_EN_RST_SHIP_MASK  0x60
#define BQ25186_SHIP_RST_EN_RST_SHIP_SHIP  0x40

/* Charge current limits reachable through bq25186_set_charge_current().
 * 5-35 mA in 1 mA steps, 40-1000 mA in 10 mA steps; there is nothing in
 * between, so requests are rejected rather than silently rounded. */
#define BQ25186_ICHG_MIN_MA   5
#define BQ25186_ICHG_MAX_MA   1000

typedef struct {
    i2c_master_bus_handle_t bus; // borrowed, created and owned by the caller
    uint16_t device_address;     // BQ25186_I2C_ADDR_DEFAULT unless the board says otherwise
    uint32_t scl_speed_hz;
    int timeout_ms;              // per transaction

    /* The /CE pin, if the firmware drives it. Left alone when drive_ce_pin is
     * false, which is what a zero-initialised config asks for - the safe
     * default, since a wrong pin number here would drive an unrelated net.
     *
     * /CE is active low: low or floating enables charging while VIN is valid,
     * high disables it. The chip's own 5 MOhm pulldown means "floating" is
     * "enabled" - but this board fits a 10 kOhm pullup, which wins by three
     * orders of magnitude, so on this board charging is disabled until
     * something drives the pin low. See bq25186_set_ce_pin(). */
    int ce_gpio;
    bool drive_ce_pin;
} bq25186_config_t;

/* Raw status registers. Deliberately not decoded into named flags yet: the
 * individual bit meanings still need a pass against the datasheet, and a wrong
 * flag name is worse than a raw byte. Use bq25186_dump_registers() during
 * bring-up to compare against the documented reset values. */
typedef struct {
    uint8_t stat0;
    uint8_t stat1;
    uint8_t flag0;
} bq25186_status_t;

/* Adds the charger as a device on the given bus. Does not touch any register. */
esp_err_t bq25186_init(const bq25186_config_t *config);

esp_err_t bq25186_deinit(void);

/* Reads STAT0 to confirm the chip acknowledges on the bus. Run this before the
 * first write - a NACK here means wiring or address, not configuration. */
esp_err_t bq25186_probe(void);

esp_err_t bq25186_read_reg(uint8_t reg, uint8_t *value);
esp_err_t bq25186_write_reg(uint8_t reg, uint8_t value);

/* Read-modify-write. Only the bits set in mask are taken from value. */
esp_err_t bq25186_update_reg(uint8_t reg, uint8_t mask, uint8_t value);

/* Logs every register from 0x00 to 0x0C at INFO level. Bring-up aid: compare
 * the output against the datasheet's reset values to prove the bus works
 * before trusting any write. */
esp_err_t bq25186_dump_registers(void);

/* Sets the fast charge current and clears CHG_DIS. Returns
 * ESP_ERR_INVALID_ARG for a value the hardware cannot encode. */
esp_err_t bq25186_set_charge_current(uint16_t milliamps);
esp_err_t bq25186_get_charge_current(uint16_t *milliamps);

/* CHG_DIS in ICHG_CTRL - the register half of the charge enable. */
esp_err_t bq25186_set_charging_enabled(bool enabled);

/* The /CE pin - the other, independent half. Both have to allow charging for
 * current to flow, and they fail differently: CHG_DIS is self-healing, because
 * the watchdog resets it to "enabled" after 160 s, while the pin keeps whatever
 * it was driven to.
 *
 * `enabled` true drives the asserted (low) level, false drives high. Not
 * released to the pullup on purpose: a driven level is one that can be read
 * back and reasoned about.
 *
 * Cost on this board: holding the pin low fights the 10 kOhm pullup and draws
 * ~330 uA for as long as it is low. That is nothing next to a charge current
 * and 33x the entire deep-sleep budget, so the caller must not leave it enabled
 * when VIN is gone - which is also when /CE has no effect anyway.
 *
 * Returns ESP_ERR_INVALID_STATE if the config did not ask for the pin to be
 * driven. */
esp_err_t bq25186_set_ce_pin(bool enabled);

/* Locks the /CE pad at its current level so it survives deep sleep, where
 * digital pads otherwise go high-impedance and the board's pull-up would take
 * over - disabling charging for the whole sleep interval, i.e. almost always.
 *
 * On targets that hold a single IO through deep sleep on their own - the C6
 * does, SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP - this is all that is needed.
 * Elsewhere the caller also has to arm gpio_deep_sleep_hold_en(). Releasing
 * happens in bq25186_init(), because a held pad ignores gpio_config() and the
 * symptom would be a /CE that never moves again after the first wake.
 *
 * Hold it only while charging is enabled. Held low with VIN gone, the pull-up
 * keeps drawing ~330 uA until something notices. */
esp_err_t bq25186_ce_pin_hold(bool hold);

esp_err_t bq25186_read_status(bq25186_status_t *status);

/* CHG_STAT, the two bits of STAT0 that say what the charger is doing. */
typedef enum {
    BQ25186_CHARGE_IDLE = 0,        // 2b00: charging enabled but no valid input, or suspended
    BQ25186_CHARGE_CONSTANT_CURRENT, // 2b01: trickle, pre-charge or fast charge - not distinguished
    BQ25186_CHARGE_CONSTANT_VOLTAGE, // 2b10: topping off
    BQ25186_CHARGE_DONE_OR_DISABLED, // 2b11: full, or the host turned charging off
} bq25186_charge_state_t;

/* Decoded STAT0. `vin_good` is the charger's own view of the input, which is
 * worth having next to the board's VIN_PGOOD pin: the two disagreeing means the
 * pin is not measuring what it is thought to measure.
 *
 * BQ25186_CHARGE_DONE_OR_DISABLED is genuinely ambiguous in the hardware - read
 * CHG_DIS from ICHG_CTRL to tell "full" from "we disabled it". */
esp_err_t bq25186_read_charge_state(bq25186_charge_state_t *state, bool *vin_good);

/* True only while current is actually going into the cell. */
bool bq25186_charge_state_is_charging(bq25186_charge_state_t state);

/* For log lines: "constant current", "done or disabled", ... */
const char *bq25186_charge_state_name(bq25186_charge_state_t state);

/* Opens the BATFET. On this board that removes the system rail, so the ESP32
 * loses power inside this call - it returns only if the write itself failed.
 * Everything that must still happen has to happen before it. */
esp_err_t bq25186_enter_ship_mode(void);

#ifdef __cplusplus
}
#endif
