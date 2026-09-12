/* One measurement cycle's worth of data.
 *
 * Its own header because two sides share it and neither owns it: app_run.c
 * fills it in, app_net.c turns it into a payload, and later the e-ink screen
 * will read the same struct. Millivolts are kept alongside the percentage
 * because the percentage depends on a calibration that is still provisional -
 * the raw number is the one that stays true.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t sequence;  // measurements since boot, 1 for the first
    int soil_mv;        // -1 when the reading failed
    int soil_percent;   // -1 when there is no valid reading or no calibration
    int battery_mv;     // -1 when the reading failed
    bool usb_present;   // charger reports VIN good
    bool charging;      // charger reports a charge in progress
} app_sample_t;
