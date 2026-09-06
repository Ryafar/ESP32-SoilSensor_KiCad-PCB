#include "soil_test_task.h"

#include <stdint.h>
#include <stdlib.h>

#include "config.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "soil_test";

#define POLL_PERIOD_MS 5000

/* Offsets after the rail switches on, in milliseconds. Deliberately dense below
 * 100 ms: that is where a capacitive probe's output filter lives, and where a
 * wrong settle time hides. The last one doubles as "this is the final value". */
static const int RAMP_OFFSETS_MS[] = {0, 5, 10, 20, 50, 100, 200, 500, 1000, 2000};
#define RAMP_STEPS (sizeof(RAMP_OFFSETS_MS) / sizeof(RAMP_OFFSETS_MS[0]))

/* How close to the final value counts as settled, when reporting which offset
 * would have been enough. Loose on purpose - the probe's own noise is larger
 * than any tolerance worth chasing here. */
#define SETTLED_TOLERANCE_MV 20

static adc_channel_t s_channel;
static adc_cali_handle_t s_cali;
static int s_min_mv = INT32_MAX;
static int s_max_mv = INT32_MIN;

static void rail_set(const soil_test_config_t *config, bool on)
{
    const int level = config->power_active_low ? (on ? 0 : 1) : (on ? 1 : 0);
    gpio_set_level(config->power_gpio, level);
}

/* pdMS_TO_TICKS truncates, so a settle shorter than one tick (10 ms at
 * CONFIG_FREERTOS_HZ=100) would not delay at all - the trap that made the
 * battery divider read its own stale node voltage. */
static void settle(void)
{
    const TickType_t ticks = pdMS_TO_TICKS(SOIL_SETTLE_MS);
    vTaskDelay(ticks == 0 ? 1 : ticks);
}

/* The interesting part of the ramp is below one tick, which vTaskDelay cannot
 * resolve, so short waits are busy-waited. This runs once and nothing else in
 * this build needs the CPU meanwhile. */
static void wait_until_us(int64_t deadline_us)
{
    for (;;) {
        const int64_t remaining = deadline_us - esp_timer_get_time();
        if (remaining <= 0) {
            return;
        }
        if (remaining > 20000) {
            /* Give the scheduler the bulk of it, keep the last two ticks for
             * the busy-wait so the offset stays accurate. */
            vTaskDelay(pdMS_TO_TICKS(remaining / 1000 - 10));
        } else {
            esp_rom_delay_us((uint32_t)remaining);
            return;
        }
    }
}

static int raw_to_mv(int raw)
{
    int mv = 0;
    if (s_cali != NULL && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
        return mv;
    }
    return raw * 3100 / 4095;
}

/* One conversion, no averaging - used for the ramp, where the transient is the
 * point and averaging would smear it. */
static int read_once_mv(adc_oneshot_unit_handle_t adc)
{
    int raw = 0;
    if (adc_oneshot_read(adc, s_channel, &raw) != ESP_OK) {
        return -1;
    }
    return raw_to_mv(raw);
}

static int read_averaged_mv(adc_oneshot_unit_handle_t adc)
{
    int raw = 0;

    /* First conversion after the channel has been idle is the least trustworthy;
     * same reasoning as the battery task. */
    if (adc_oneshot_read(adc, s_channel, &raw) != ESP_OK) {
        return -1;
    }

    int sum = 0;
    for (int i = 0; i < SOIL_ADC_SAMPLES; i++) {
        if (adc_oneshot_read(adc, s_channel, &raw) != ESP_OK) {
            return -1;
        }
        sum += raw;
    }
    return raw_to_mv(sum / SOIL_ADC_SAMPLES);
}

/* Powers the rail, samples the ramp, and reports where the output flattened.
 * Runs once, before the measurement loop starts trusting SOIL_SETTLE_MS. */
static void characterise_settle_time(const soil_test_config_t *config)
{
    rail_set(config, false);
    settle();
    const int off_mv = read_averaged_mv(config->adc);
    ESP_LOGI(TAG, "rail off: %d mV", off_mv);

    int ramp_mv[RAMP_STEPS];
    const int64_t start_us = esp_timer_get_time();
    rail_set(config, true);

    for (size_t i = 0; i < RAMP_STEPS; i++) {
        wait_until_us(start_us + (int64_t)RAMP_OFFSETS_MS[i] * 1000);
        const int64_t actual_us = esp_timer_get_time() - start_us;
        ramp_mv[i] = read_once_mv(config->adc);
        ESP_LOGI(TAG, "  t = %4d ms (actual %5lld us): %d mV", RAMP_OFFSETS_MS[i],
                 actual_us, ramp_mv[i]);
    }

    rail_set(config, false);

    const int final_mv = ramp_mv[RAMP_STEPS - 1];
    if (final_mv < 0) {
        ESP_LOGE(TAG, "the last ramp sample failed - nothing to conclude from");
        return;
    }

    /* A rail that never switched looks like a ramp that starts where it ends. */
    if (off_mv >= 0 && abs(final_mv - off_mv) <= SETTLED_TOLERANCE_MV) {
        ESP_LOGE(TAG, "powered and unpowered read the same (%d vs %d mV) - the "
                      "rail is not switching, the probe is not plugged into "
                      "CON2, or SOIL_MEAS is not on GPIO%d",
                 off_mv, final_mv, config->meas_gpio);
        return;
    }

    int settled_at = -1;
    for (size_t i = 0; i < RAMP_STEPS; i++) {
        if (ramp_mv[i] >= 0 && abs(ramp_mv[i] - final_mv) <= SETTLED_TOLERANCE_MV) {
            settled_at = RAMP_OFFSETS_MS[i];
            break;
        }
    }

    if (settled_at < 0) {
        ESP_LOGW(TAG, "still moving at %d ms - the probe needs longer than this "
                      "ramp covers, extend RAMP_OFFSETS_MS",
                 RAMP_OFFSETS_MS[RAMP_STEPS - 1]);
        return;
    }

    ESP_LOGI(TAG, "settled within %d mV of %d mV after %d ms; SOIL_SETTLE_MS is "
                  "%d ms", SETTLED_TOLERANCE_MV, final_mv, settled_at,
             SOIL_SETTLE_MS);
    if (settled_at > SOIL_SETTLE_MS) {
        ESP_LOGE(TAG, "SOIL_SETTLE_MS is too short - every reading below is "
                      "taken before the probe has settled. Raise it in "
                      "main/config.h to at least %d ms", settled_at);
    }
}

static void configure_pins(const soil_test_config_t *config)
{
    const gpio_config_t power_config = {
        .pin_bit_mask = 1ULL << config->power_gpio,
        /* INPUT_OUTPUT rather than OUTPUT: it keeps the input buffer on, so
         * gpio_get_level reports the pad's actual level instead of echoing what
         * we asked for. A pin held by something else is then visible. */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&power_config));

    /* Off first. The pin floats out of reset, so until this runs the P-FET gate
     * is undefined and the probe may well be powered. */
    rail_set(config, false);
}

static void setup_adc(const soil_test_config_t *config)
{
    adc_unit_t unit;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(config->meas_gpio, &unit, &s_channel));
    ESP_LOGI(TAG, "SOIL_MEAS on GPIO%d is ADC%d channel %d, rail on GPIO%d "
                  "(active %s)", config->meas_gpio, unit + 1, s_channel,
             config->power_gpio, config->power_active_low ? "low" : "high");

    const adc_oneshot_chan_cfg_t chan_config = {
        /* The probe swings across most of the 3.3 V rail, so the widest
         * attenuation is the only one that fits. */
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(config->adc, s_channel, &chan_config));

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = unit,
        .chan = s_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "no calibration data in efuse, readings will be rough");
        s_cali = NULL;
    }
#else
    ESP_LOGW(TAG, "curve fitting calibration not supported on this target");
#endif
}

void soil_test_task(void *arg)
{
    const soil_test_config_t *config = (const soil_test_config_t *)arg;

    configure_pins(config);
    setup_adc(config);

    ESP_LOGI(TAG, "characterising the probe's settle time after power-up");
    characterise_settle_time(config);

    while (1) {
        rail_set(config, true);
        settle();
        const int mv = read_averaged_mv(config->adc);
        rail_set(config, false);

        if (mv < 0) {
            ESP_LOGW(TAG, "conversion failed");
        } else {
            if (mv < s_min_mv) {
                s_min_mv = mv;
            }
            if (mv > s_max_mv) {
                s_max_mv = mv;
            }
            /* min/max accumulate across the session: hold the probe in air, then
             * in water, and the two ends of the calibration curve are in the
             * log without any further code (§9 open item). */
            ESP_LOGI(TAG, "soil %d mV   (seen %d..%d mV)", mv, s_min_mv, s_max_mv);
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

/* Half period of the rail toggle. Slow on purpose: a hand-held meter needs to
 * settle, and the point here is watching a voltage with your eyes, not logging
 * a fast edge. */
#define TOGGLE_HALF_PERIOD_MS 2000

void soil_rail_toggle_task(void *arg)
{
    const soil_test_config_t *config = (const soil_test_config_t *)arg;

    configure_pins(config);
    setup_adc(config);

    ESP_LOGI(TAG, "toggling SOIL_POWER on GPIO%d every %d ms - on drives %d, "
                  "off drives %d. Meter on the probe's supply pin at CON2 "
                  "should follow", config->power_gpio, TOGGLE_HALF_PERIOD_MS,
             config->power_active_low ? 0 : 1, config->power_active_low ? 1 : 0);

    bool on = false;
    while (1) {
        on = !on;
        rail_set(config, on);

        /* Measure at the end of the half period, so the reading is fully
         * settled whatever the probe's time constant turns out to be. */
        vTaskDelay(pdMS_TO_TICKS(TOGGLE_HALF_PERIOD_MS));

        const int pad = gpio_get_level(config->power_gpio);
        const int mv = read_averaged_mv(config->adc);
        ESP_LOGI(TAG, "rail %s: GPIO%d pad reads %d, SOIL_MEAS %d mV",
                 on ? "ON " : "OFF", config->power_gpio, pad, mv);
    }
}
