#include "soil.h"

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "soil";

/* Nominal full scale of ADC_ATTEN_DB_12, used only when the chip carries no
 * calibration data in efuse. */
#define UNCALIBRATED_FULL_SCALE_MV 3100
#define ADC_MAX_RAW 4095

static soil_config_t s_config;
static adc_channel_t s_channel;
static adc_cali_handle_t s_cali;
static bool s_initialised;

static void rail_set(bool on)
{
    const int level = s_config.power_active_low ? (on ? 0 : 1) : (on ? 1 : 0);
    gpio_set_level(s_config.power_gpio, level);
}

/* pdMS_TO_TICKS truncates: a settle below one tick (10 ms at
 * CONFIG_FREERTOS_HZ=100) would not delay at all, and the reading would be the
 * probe's output from before it was powered. */
static void settle(void)
{
    const TickType_t ticks = pdMS_TO_TICKS(s_config.settle_ms);
    vTaskDelay(ticks == 0 ? 1 : ticks);
}

static esp_err_t read_averaged_mv(int *out_mv)
{
    int raw = 0;

    /* First conversion after the channel has been idle is the least
     * trustworthy; discarded rather than averaged in. */
    ESP_RETURN_ON_ERROR(adc_oneshot_read(s_config.adc, s_channel, &raw), TAG,
                        "discard conversion failed");

    int sum = 0;
    for (int i = 0; i < s_config.samples; i++) {
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_config.adc, s_channel, &raw), TAG,
                            "conversion %d failed", i);
        sum += raw;
    }
    const int average_raw = sum / s_config.samples;

    if (s_cali != NULL && adc_cali_raw_to_voltage(s_cali, average_raw, out_mv) == ESP_OK) {
        return ESP_OK;
    }
    *out_mv = average_raw * UNCALIBRATED_FULL_SCALE_MV / ADC_MAX_RAW;
    return ESP_OK;
}

/* Wet reads low and dry reads high, so the percentage runs against the voltage.
 * Clamped, because a probe in water colder than the calibration water will step
 * outside the two points, and 103 % is a number nobody wants to explain. */
static int to_percent(int mv)
{
    const int span = s_config.dry_mv - s_config.wet_mv;
    if (span <= 0) {
        return -1; // calibration is nonsense; say so rather than divide by it
    }
    const int percent = (s_config.dry_mv - mv) * 100 / span;
    if (percent < 0) {
        return 0;
    }
    if (percent > 100) {
        return 100;
    }
    return percent;
}

static esp_err_t configure_pins(void)
{
    const gpio_config_t power_config = {
        .pin_bit_mask = 1ULL << s_config.power_gpio,
        /* INPUT_OUTPUT rather than OUTPUT: the input buffer stays on, so
         * gpio_get_level reports the pad's real level instead of echoing what
         * was written. A pin held by something else is then visible. */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&power_config), TAG, "rail gpio config failed");
    rail_set(false);
    return ESP_OK;
}

static esp_err_t configure_adc(void)
{
    adc_unit_t unit;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(s_config.meas_gpio, &unit, &s_channel),
                        TAG, "GPIO%d is not an ADC pin", s_config.meas_gpio);

    const adc_oneshot_chan_cfg_t chan_config = {
        /* The probe swings across most of the 3.3 V rail, so the widest
         * attenuation is the only one that fits. */
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_config.adc, s_channel, &chan_config),
                        TAG, "channel config failed");

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
#endif
    return ESP_OK;
}

esp_err_t soil_init(const soil_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "no config");
    ESP_RETURN_ON_FALSE(config->adc != NULL, ESP_ERR_INVALID_ARG, TAG, "no adc handle");
    ESP_RETURN_ON_FALSE(config->samples > 0, ESP_ERR_INVALID_ARG, TAG, "samples must be > 0");

    s_config = *config;
    if (s_config.dry_mv <= s_config.wet_mv) {
        /* Not fatal: the millivolts are still worth logging, and a missing
         * percentage is better spotted as a -1 than as a plausible number. */
        ESP_LOGW(TAG, "dry (%d mV) is not above wet (%d mV) - percent will read -1",
                 s_config.dry_mv, s_config.wet_mv);
    }

    ESP_RETURN_ON_ERROR(configure_pins(), TAG, "pin setup failed");
    ESP_RETURN_ON_ERROR(configure_adc(), TAG, "adc setup failed");
    s_initialised = true;
    return ESP_OK;
}

esp_err_t soil_deinit(void)
{
    if (!s_initialised) {
        return ESP_OK;
    }
    rail_set(false);
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (s_cali != NULL) {
        adc_cali_delete_scheme_curve_fitting(s_cali);
        s_cali = NULL;
    }
#endif
    s_initialised = false;
    return ESP_OK;
}

esp_err_t soil_read(soil_reading_t *out)
{
    ESP_RETURN_ON_FALSE(s_initialised, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "no output");

    rail_set(true);
    settle();
    int mv = 0;
    const esp_err_t err = read_averaged_mv(&mv);
    /* Before the error check - the rail goes off even when the conversion
     * failed. */
    rail_set(false);
    ESP_RETURN_ON_ERROR(err, TAG, "reading the probe failed");

    out->millivolts = mv;
    out->percent = to_percent(mv);
    return ESP_OK;
}
