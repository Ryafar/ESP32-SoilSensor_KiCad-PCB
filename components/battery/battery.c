#include "battery.h"

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "battery";

/* What a 1-cell LiPo can physically be: 2.4 V is past deeply flat, 4.4 V is
 * above any charge voltage the BQ25186 produces. A reading outside this is not
 * a cell voltage, it is the ADC pin floating - which reads near full scale,
 * because the node keeps whatever charge is on the 100 nF. */
#define CELL_MIN_MV 2400
#define CELL_MAX_MV 4400

/* Nominal full scale of ADC_ATTEN_DB_12, used only when the chip carries no
 * calibration data in efuse. Good enough to tell "connected" from "not
 * connected", not good enough to calibrate the divider against. */
#define UNCALIBRATED_FULL_SCALE_MV 3100
#define ADC_MAX_RAW 4095

static battery_config_t s_config;
static adc_channel_t s_channel;
static adc_cali_handle_t s_cali;
static bool s_initialised;

static void enable_divider(bool on)
{
    const int level = s_config.enable_active_high ? (on ? 1 : 0) : (on ? 0 : 1);
    gpio_set_level(s_config.enable_gpio, level);
}

/* pdMS_TO_TICKS truncates, so a settle time below one tick would not delay at
 * all and the conversion would return the node's previous voltage - a plausible
 * number that is quietly wrong. */
static void settle(void)
{
    const TickType_t ticks = pdMS_TO_TICKS(s_config.settle_ms);
    vTaskDelay(ticks == 0 ? 1 : ticks);
}

static esp_err_t read_adc_mv(int *out_mv)
{
    int raw = 0;

    /* The 100 nF across the lower leg recharges after the switch closes, so the
     * first conversion is thrown away rather than averaged in. */
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

static esp_err_t configure_pins(void)
{
    const gpio_config_t enable_config = {
        .pin_bit_mask = 1ULL << s_config.enable_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&enable_config), TAG, "enable gpio config failed");
    enable_divider(false);

    /* The board fits 10 kOhm pull-ups on both lines, so the internal ones stay
     * off on purpose: enabling them would hide an unfitted external pull-up
     * behind a level that looks correct. */
    const gpio_config_t status_config = {
        .pin_bit_mask = (1ULL << s_config.int_gpio) | (1ULL << s_config.power_good_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&status_config);
}

static esp_err_t configure_adc(void)
{
    adc_unit_t unit;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(s_config.meas_gpio, &unit, &s_channel),
                        TAG, "GPIO%d is not an ADC pin", s_config.meas_gpio);

    const adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN_DB_12, // 2.1 V at a full cell fits with headroom
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

esp_err_t battery_init(const battery_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "no config");
    ESP_RETURN_ON_FALSE(config->adc != NULL, ESP_ERR_INVALID_ARG, TAG, "no adc handle");
    ESP_RETURN_ON_FALSE(config->samples > 0, ESP_ERR_INVALID_ARG, TAG, "samples must be > 0");
    ESP_RETURN_ON_FALSE(config->divider_ratio > 0.0f, ESP_ERR_INVALID_ARG, TAG,
                        "divider ratio must be > 0");

    s_config = *config;
    ESP_RETURN_ON_ERROR(configure_pins(), TAG, "pin setup failed");
    ESP_RETURN_ON_ERROR(configure_adc(), TAG, "adc setup failed");
    s_initialised = true;
    return ESP_OK;
}

esp_err_t battery_deinit(void)
{
    if (!s_initialised) {
        return ESP_OK;
    }
    enable_divider(false);
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (s_cali != NULL) {
        adc_cali_delete_scheme_curve_fitting(s_cali);
        s_cali = NULL;
    }
#endif
    s_initialised = false;
    return ESP_OK;
}

esp_err_t battery_read(battery_reading_t *out)
{
    ESP_RETURN_ON_FALSE(s_initialised, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "no output");

    /* Both are open-drain with a pull-up: low means asserted. Read before the
     * divider is switched in, so the sample is not taken while the ADC node is
     * still charging. */
    out->usb_present = gpio_get_level(s_config.power_good_gpio) == 0;
    out->charger_int = gpio_get_level(s_config.int_gpio) == 0;

    enable_divider(true);
    settle();
    int adc_mv = 0;
    const esp_err_t err = read_adc_mv(&adc_mv);
    /* Before the error check: the divider goes off on every exit path, or the
     * board draws ~21 uA forever over a failed conversion. */
    enable_divider(false);
    ESP_RETURN_ON_ERROR(err, TAG, "reading the divider failed");

    const int cell_mv = (int)(adc_mv * s_config.divider_ratio);

    /* Which side of the window it failed on says which fault it is, and the two
     * are nothing alike - so they get separate messages rather than one that
     * guesses. Too low means the node is being pulled to ground: no cell in the
     * holder. Too high means it is floating at its own charge, which is what a
     * divider that never switched in looks like. */
    ESP_RETURN_ON_FALSE(cell_mv >= CELL_MIN_MV, ESP_ERR_INVALID_STATE, TAG,
                        "%d mV at the cell (%d mV at the ADC) is below anything a "
                        "LiPo can be - no cell attached, or it is deeply flat",
                        cell_mv, adc_mv);
    ESP_RETURN_ON_FALSE(cell_mv <= CELL_MAX_MV, ESP_ERR_INVALID_STATE, TAG,
                        "%d mV at the cell (%d mV at the ADC) is above anything a "
                        "LiPo can be - that is the ADC node floating, i.e. the "
                        "divider did not switch in", cell_mv, adc_mv);

    out->millivolts = cell_mv;
    return ESP_OK;
}
