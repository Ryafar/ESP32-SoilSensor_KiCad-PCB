/* One measurement cycle's worth of data.
 *
 * Its own header because several sides share it and none owns it: app_run.c
 * fills it in, app_buffer.c keeps it in RTC memory until it can be sent,
 * app_net.c turns it into a payload, and app_screen.c draws it. Millivolts are
 * kept alongside the percentage because the percentage depends on a calibration
 * that is still provisional - the raw number is the one that stays true.
 *
 * `taken_at_s` is the RTC clock, not wall time: there is no SNTP yet, so the
 * epoch is "since the last cold boot". It is still the right thing to store,
 * because the difference between two of them is valid either way - that is what
 * lets a buffered sample be published with its age rather than a timestamp
 * nobody can interpret (product design §5).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t sequence;  // measurements since the last cold boot, 1 for the first
    uint32_t taken_at_s; // seconds on the RTC clock when this was measured
    int soil_mv;        // -1 when the reading failed
    int soil_percent;   // -1 when there is no valid reading or no calibration
    int battery_mv;     // -1 when the reading failed
    bool usb_present;   // charger reports VIN good
    bool charging;      // charger reports a charge in progress
} app_sample_t;
