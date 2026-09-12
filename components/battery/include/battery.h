/* Cell voltage behind the 1:2 divider on BAT_MEAS, plus the charger's two
 * status lines.
 *
 * The divider's low leg hangs on a BSS138 so the resistors only draw current
 * while a reading is taken. This driver owns that switch: every reading asserts
 * it, waits out the RC, converts, and releases it again - including on the
 * error paths. A caller cannot leave the divider connected.
 *
 * The ADC unit is borrowed, not owned: battery and soil sit on two channels of
 * the same ADC1, and a unit can only be created once. The entry point creates
 * it and lends the handle.
 *
 * Board-agnostic: every pin and the divider ratio arrive through
 * battery_config_t, which main/config.h fills in.
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
    int meas_gpio;                 // ADC input behind the divider
    int enable_gpio;               // BSS138 gate, connects the divider's low leg
    int int_gpio;                  // charger INT, active low
    int power_good_gpio;           // charger PG, low while VIN is good
    bool enable_active_high;       // level on enable_gpio that connects the divider
    float divider_ratio;           // cell voltage / ADC voltage, nominally 2.0
    int settle_ms;                 // after asserting enable, before converting
    int samples;                   // conversions averaged per reading
} battery_config_t;

typedef struct {
    int millivolts;  // at the cell, divider already undone
    bool usb_present; // VIN_PGOOD asserted
    bool charger_int; // charger is pulling INT low
} battery_reading_t;

/* Configures the enable output, the two status inputs and the ADC channel. The
 * divider is disconnected when this returns. */
esp_err_t battery_init(const battery_config_t *config);

esp_err_t battery_deinit(void);

/* One averaged reading. Returns ESP_ERR_INVALID_STATE when the voltage is
 * outside anything a 1-cell LiPo can be - that means the divider never switched
 * in, and the alternative is doubling a floating node into a confident-looking
 * 7 V. Bring-up produced exactly that, which is why the check is in the driver
 * rather than in the caller. */
esp_err_t battery_read(battery_reading_t *out);

#ifdef __cplusplus
}
#endif
