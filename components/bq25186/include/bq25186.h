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

/* Register map. Addresses are from the datasheet's register summary; the bit
 * level detail below is only spelled out where it has been cross-checked - see
 * the note on the watchdog in bq25186_configure_charging(). */
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

esp_err_t bq25186_set_charging_enabled(bool enabled);

esp_err_t bq25186_read_status(bq25186_status_t *status);

/* Opens the BATFET. On this board that removes the system rail, so the ESP32
 * loses power inside this call - it returns only if the write itself failed.
 * Everything that must still happen has to happen before it. */
esp_err_t bq25186_enter_ship_mode(void);

#ifdef __cplusplus
}
#endif
