/* Bring-up task for battery voltage sensing and the charger's status pins.
 *
 * Three questions:
 *   1. Which BAT_MEAS_EN level actually switches the divider in? The BSS138
 *      polarity is an open item, and the test determines it instead of
 *      assuming it - a divider that is permanently on costs 21 uA forever,
 *      which is seven times what the whole board draws in ship mode. Nothing
 *      else would ever make that visible.
 *   2. Does the measured voltage match a multimeter? That calibrates
 *      BATTERY_DIVIDER_RATIO.
 *   3. What do INT and POWER_GOOD do when USB or the cell is plugged in? Both
 *      are only read here, never driven.
 *
 * CHARGE_EN is deliberately left alone: it is an output that changes charger
 * behaviour, and this task is read-only.
 *
 * The ADC unit is borrowed - main creates it, because soil sampling will later
 * need the same unit and only one owner can exist.
 */
#pragma once

#include "esp_adc/adc_oneshot.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    adc_oneshot_unit_handle_t adc; // borrowed from main
    int meas_gpio;                 // BAT_MEAS, behind the divider
    int enable_gpio;               // BAT_MEAS_EN, switches the divider in
    int int_gpio;                  // charger INT, active low
    int power_good_gpio;           // charger POWER_GOOD
    float divider_ratio;           // ADC volts -> battery volts
} battery_test_config_t;

/* Pass a pointer to a battery_test_config_t that outlives the task. */
void battery_test_task(void *arg);

#ifdef __cplusplus
}
#endif
