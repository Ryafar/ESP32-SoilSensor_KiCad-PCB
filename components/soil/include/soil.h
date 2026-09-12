/* Capacitive soil probe on CON2.
 *
 * The probe is only powered while it is being read: SOIL_POWER switches its
 * rail through a P-FET, and this driver owns that switch the same way the
 * battery driver owns BAT_MEAS_EN. Rail on, settle, average, rail off - on
 * every exit path, error paths included. A probe left powered is a battery-life
 * bug that nothing in the log would show.
 *
 * The ADC unit is borrowed, not owned: soil and battery are two channels of the
 * same ADC1.
 *
 * Calibration is two points, dry and wet, passed in at init. The mapping
 * between them is linear and clamped - not physically exact, but the number is
 * a gardening aid, not an instrument reading.
 */
#pragma once

#include <stdbool.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    adc_oneshot_unit_handle_t adc; // borrowed, created by the entry point
    int meas_gpio;                 // ADC input from the probe
    int power_gpio;                // load switch for the probe rail
    bool power_active_low;         // AO3401A is a P-FET: gate to GND conducts
    int settle_ms;                 // after switching the rail on, before converting
    int samples;                   // conversions averaged per reading
    int dry_mv;                    // probe in air, the high end
    int wet_mv;                    // probe submerged, the low end
} soil_config_t;

typedef struct {
    int millivolts; // what the probe actually output, the honest number
    int percent;    // 0-100 between wet_mv and dry_mv, -1 if uncalibrated
} soil_reading_t;

/* Drives the rail to its OFF level before anything else - a GPIO comes out of
 * reset floating, which leaves the P-FET gate undefined and the probe possibly
 * powered. */
esp_err_t soil_init(const soil_config_t *config);

/* Releases the rail. */
esp_err_t soil_deinit(void);

esp_err_t soil_read(soil_reading_t *out);

#ifdef __cplusplus
}
#endif
