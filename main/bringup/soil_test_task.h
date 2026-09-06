/* Bring-up task for the capacitive soil probe on CON2.
 *
 * Three questions, in the order they have to be answered:
 *   1. Does SOIL_POWER actually switch the probe rail? With the rail off the
 *      probe is unpowered and its output should collapse. A reading that does
 *      not change between off and on means the AO3401A is not switching, the
 *      probe is not plugged in, or GPIO14 is not the pin we think it is.
 *   2. How long does the probe need after power-up? docs/hardware/README.md §6
 *      says "~tens of ms", which is a guess, not a measurement. So the task
 *      samples a ramp at fixed offsets after switching the rail on and logs the
 *      curve - read off where it flattens and put that into SOIL_SETTLE_MS.
 *      Sampling too early is the failure mode that produces plausible numbers
 *      that are quietly wrong, which is exactly what happened on the battery
 *      divider.
 *   3. What range does the probe cover? §9 still lists this as open. The loop
 *      logs the running minimum and maximum, so dunking the probe in water and
 *      holding it in air fills in both calibration points without any extra
 *      code.
 *
 * The rail is driven to its OFF level before anything else, because an ESP32
 * GPIO comes up floating and the P-FET gate would otherwise be undefined.
 *
 * The ADC unit is borrowed from main, which also lends it to the battery task.
 *
 * Never returns.
 */
#pragma once

#include <stdbool.h>

#include "esp_adc/adc_oneshot.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    adc_oneshot_unit_handle_t adc; // borrowed from main
    int meas_gpio;                 // SOIL_MEAS, the probe's analog output
    int power_gpio;                // SOIL_POWER, the probe rail load switch
    bool power_active_low;         // AO3401A P-FET: drive low to power the rail
} soil_test_config_t;

/* Pass a pointer to a soil_test_config_t that outlives the task. */
void soil_test_task(void *arg);

/* Alternative to soil_test_task, for a meter rather than a log: toggles
 * SOIL_POWER slowly and logs, per half period, the level read back off the pad
 * and what SOIL_MEAS sees. Answers "does the AO3401A switch at all" without any
 * ADC interpretation - put a meter on the probe's supply at CON2 and watch it
 * follow. Takes the same soil_test_config_t. Never returns. */
void soil_rail_toggle_task(void *arg);

#ifdef __cplusplus
}
#endif
