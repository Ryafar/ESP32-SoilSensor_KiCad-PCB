/* I2C bring-up diagnostics.
 *
 * Three questions, in the order worth asking them:
 *   1. Can the two lines even sit high?      i2c_report_line_levels()
 *   2. Is a slave holding SDA down?          i2c_bus_recover()
 *   3. Is anything out there, at any address? i2c_scan_pin_orientations()
 *
 * Question 1 comes first because a NACK and a timeout are different failures.
 * A NACK means the bus works and nobody lives at that address. A timeout means
 * the transaction never happened, which almost always means a line is stuck
 * low - and then no amount of address hunting will help.
 */
#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Releases both pins, enables the internal pull-ups and logs the resulting
 * levels. Both should read 1. A 0 means that line is held low by something
 * else - a stuck slave, a solder bridge to ground, or a pin that is not the
 * one the schematic says it is.
 *
 * Then drives each line low individually and checks the other one stays high,
 * which catches an SDA/SCL short.
 */
void i2c_report_line_levels(int sda, int scl);

/* Standard I2C recovery: if SDA is low, clock SCL up to nine times so a slave
 * stuck mid-byte can finish its transfer, then issue a STOP. Returns true if
 * SDA is high afterwards.
 *
 * Worth trying before concluding the hardware is broken - a charger that was
 * interrupted mid-transaction by a reset holds SDA low forever otherwise.
 */
bool i2c_bus_recover(int sda, int scl);

/* Probes every 7-bit address from 0x08 to 0x77 on an existing bus and logs
 * each one that acknowledges. */
void i2c_scan_bus(i2c_master_bus_handle_t bus, const char *label);

/* Creates a temporary bus, scans it, tears it down - once with the given pin
 * assignment and once with SDA and SCL swapped. Leaves no bus behind. */
void i2c_scan_pin_orientations(int sda, int scl);

#ifdef __cplusplus
}
#endif
