#include "battery_test_task.h"

#include "config.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bat_test";

#define POLL_PERIOD_MS 2000

/* A reading below this is "the divider is not connected" rather than "the cell
 * is flat" - a 1-cell LiPo that is alive cannot be under 2.0 V, so half of it
 * cannot be under 1.0 V at the ADC. */
#define DIVIDER_PRESENT_THRESHOLD_MV 500

static adc_channel_t s_channel;
static adc_cali_handle_t s_cali;
static int s_enable_asserted_level = -1; // determined by the polarity probe

/* Averaged reading in millivolts at the ADC pin, i.e. before the divider is
 * undone. Returns -1 if the conversion failed. */
static int read_adc_mv(adc_oneshot_unit_handle_t adc)
{
    int raw = 0;

    /* The 100 nF across the lower divider leg has to recharge after the switch
     * closes, so the first conversion is thrown away. */
    if (adc_oneshot_read(adc, s_channel, &raw) != ESP_OK) {
        return -1;
    }

    int sum = 0;
    for (int i = 0; i < BATTERY_ADC_SAMPLES; i++) {
        if (adc_oneshot_read(adc, s_channel, &raw) != ESP_OK) {
            return -1;
        }
        sum += raw;
    }
    const int average_raw = sum / BATTERY_ADC_SAMPLES;

    int mv = 0;
    if (s_cali != NULL && adc_cali_raw_to_voltage(s_cali, average_raw, &mv) == ESP_OK) {
        return mv;
    }

    /* No calibration available: fall back to the nominal full-scale of the 12 dB
     * attenuation. Good enough to tell "connected" from "not connected", not
     * good enough to calibrate the divider against. */
    ESP_LOGW(TAG, "uncalibrated reading, treating full scale as 3100 mV");
    return average_raw * 3100 / 4095;
}

/* Tries both enable levels and reports which one switches the divider in.
 * Returns the asserted level, or -1 if neither produced a signal. */
static int probe_enable_polarity(const battery_test_config_t *config)
{
    int mv_at[2];

    for (int level = 0; level <= 1; level++) {
        gpio_set_level(config->enable_gpio, level);
        vTaskDelay(pdMS_TO_TICKS(BATTERY_SETTLE_MS + 20));
        mv_at[level] = read_adc_mv(config->adc);
        ESP_LOGI(TAG, "BAT_MEAS_EN=%d -> %d mV at the ADC", level, mv_at[level]);
    }

    gpio_set_level(config->enable_gpio, 0);

    const bool low_reads = mv_at[0] > DIVIDER_PRESENT_THRESHOLD_MV;
    const bool high_reads = mv_at[1] > DIVIDER_PRESENT_THRESHOLD_MV;

    if (low_reads && high_reads) {
        ESP_LOGE(TAG, "both levels read a voltage - the divider is never "
                      "switched off. That is a permanent ~21 uA drain; check "
                      "the BSS138 wiring");
        /* Still usable for measuring, so carry on with the higher reading. */
        return mv_at[1] >= mv_at[0] ? 1 : 0;
    }
    if (!low_reads && !high_reads) {
        ESP_LOGE(TAG, "neither level reads a voltage - no cell attached, or "
                      "BAT_MEAS/BAT_MEAS_EN are not the pins we think they are");
        return -1;
    }

    const int asserted = high_reads ? 1 : 0;
    ESP_LOGI(TAG, "BAT_MEAS_EN asserted level is %d (%s-active)", asserted,
             asserted ? "high" : "low");
    if (asserted != BAT_MEAS_EN_ACTIVE_HIGH) {
        ESP_LOGE(TAG, "that contradicts the schematic - config.h says active high");
    }
    return asserted;
}

static void configure_pins(const battery_test_config_t *config)
{
    const gpio_config_t enable_config = {
        .pin_bit_mask = 1ULL << config->enable_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&enable_config));
    gpio_set_level(config->enable_gpio, 0);

    /* The board fits a 10 kOhm pull-up on both, so the internal ones stay off
     * on purpose: enabling them would hide a missing or unfitted external
     * pull-up behind a level that looks correct. */
    const gpio_config_t status_config = {
        .pin_bit_mask = (1ULL << config->int_gpio) | (1ULL << config->power_good_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&status_config));
}

static void setup_adc(const battery_test_config_t *config)
{
    adc_unit_t unit;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(config->meas_gpio, &unit, &s_channel));
    ESP_LOGI(TAG, "BAT_MEAS on GPIO%d is ADC%d channel %d", config->meas_gpio,
             unit + 1, s_channel);

    const adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN_DB_12, // 2.1 V at a full cell fits with headroom
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

void battery_test_task(void *arg)
{
    const battery_test_config_t *config = (const battery_test_config_t *)arg;

    configure_pins(config);
    setup_adc(config);

    /* The schematic says active high with a pull-down. The probe stays anyway -
     * it costs one extra reading and turns a wiring mistake into a log line
     * instead of a plausible-looking wrong voltage. */
    ESP_LOGI(TAG, "probing BAT_MEAS_EN polarity, expecting active high");
    s_enable_asserted_level = probe_enable_polarity(config);

    while (1) {
        int battery_mv = -1;

        if (s_enable_asserted_level >= 0) {
            gpio_set_level(config->enable_gpio, s_enable_asserted_level);
            vTaskDelay(pdMS_TO_TICKS(BATTERY_SETTLE_MS));
            const int adc_mv = read_adc_mv(config->adc);
            gpio_set_level(config->enable_gpio, !s_enable_asserted_level);

            if (adc_mv >= 0) {
                battery_mv = (int)(adc_mv * config->divider_ratio);
            }
        }

        /* One line with everything, so plugging USB or the cell in and out and
         * diffing the log is enough to work out what each signal means. */
        ESP_LOGI(TAG, "VBAT=%d mV  INT=%d  POWER_GOOD=%d", battery_mv,
                 gpio_get_level(config->int_gpio),
                 gpio_get_level(config->power_good_gpio));

        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}
