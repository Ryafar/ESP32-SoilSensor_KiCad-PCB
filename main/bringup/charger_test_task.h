/* Bring-up task for the BQ25186 charger - step 1 of the battery bring-up.
 *
 * Read-only: probes the chip on I2C, dumps every register once so the values
 * can be compared against the datasheet's reset values, then logs the status
 * registers periodically. It writes nothing, so it is safe to run with or
 * without a cell attached.
 *
 * What to look for in the output:
 *   - "charger responded" at all -> bus, address and wiring are good
 *   - reg 0x03 = 0x46  -> charge voltage at its 4.2 V default
 *   - reg 0x04 = 0x05  -> charge current still at the 10 mA default, i.e. the
 *                         charger has never been configured. Anything else
 *                         means something already wrote to it
 *   - reg 0x07         -> compare against the datasheet to settle where the
 *                         watchdog bits actually live (see bq25186.h)
 *
 * Expects bq25186_init() to have run already.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void charger_test_task(void *arg);

#ifdef __cplusplus
}
#endif
