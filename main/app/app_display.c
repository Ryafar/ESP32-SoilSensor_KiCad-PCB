#include "app_display.h"

#include "config.h"
#include "eink.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "app_display";

/* The battery is drawn as a coarse icon, so comparing raw millivolts would
 * report a change on every reading and refresh the panel for a picture that is
 * pixel for pixel identical. 50 mV is finer than the icon can show and coarse
 * enough that ADC noise does not trigger a 21-second refresh. */
#define BATTERY_COMPARE_STEP_MV 50

static bool s_have_frame;
static eink_screen_t s_shown;

/* For the log line. The colour on the glass is the component's business, but
 * which level the app decided belongs in the log next to the percentage. */
static const char *level_name(eink_moisture_level_t level)
{
    switch (level) {
    case EINK_MOISTURE_OK:
        return "ok";
    case EINK_MOISTURE_WARN:
        return "drying";
    case EINK_MOISTURE_ALERT:
        return "water it";
    }
    return "?";
}

/* A reading that failed is not dry soil - it gets the neutral level, so a
 * broken probe never draws a red alert nobody can act on. */
static eink_moisture_level_t moisture_level(int soil_percent)
{
    if (soil_percent < 0) {
        return EINK_MOISTURE_OK;
    }
    if (soil_percent <= SOIL_ALERT_PERCENT) {
        return EINK_MOISTURE_ALERT;
    }
    if (soil_percent <= SOIL_WARN_PERCENT) {
        return EINK_MOISTURE_WARN;
    }
    return EINK_MOISTURE_OK;
}

static eink_screen_t screen_from(const app_sample_t *sample, bool battery_low)
{
    return (eink_screen_t){
        .soil_percent = sample->soil_percent,
        .moisture = moisture_level(sample->soil_percent),
        .battery_mv = sample->battery_mv,
        .charging = sample->charging,
        .battery_low = battery_low,
    };
}

static bool differs(const eink_screen_t *a, const eink_screen_t *b)
{
    return a->soil_percent != b->soil_percent ||
           a->moisture != b->moisture ||
           a->charging != b->charging ||
           a->battery_low != b->battery_low ||
           a->battery_mv / BATTERY_COMPARE_STEP_MV != b->battery_mv / BATTERY_COMPARE_STEP_MV;
}

void app_display_update(const app_sample_t *sample, bool battery_low)
{
    if (sample == NULL) {
        return;
    }

    const eink_screen_t screen = screen_from(sample, battery_low);
    if (s_have_frame && !differs(&screen, &s_shown)) {
        return;
    }

    const eink_config_t config = {
        .spi_host = EPD_SPI_HOST,
        .pin_clk = PIN_EPD_CLK,
        .pin_mosi = PIN_EPD_MOSI,
        .pin_cs = PIN_EPD_CS,
        .pin_dc = PIN_EPD_DC,
        .pin_rst = PIN_EPD_RST,
        .pin_busy = PIN_EPD_BUSY,
        .pin_power = PIN_EPD_POWER,
        .power_active_low = EPD_POWER_ACTIVE_LOW,
    };

    esp_err_t err = eink_init(&config);
    if (err != ESP_OK) {
        /* Not fatal and not retried here: the measurement is already in the log
         * and on its way to MQTT, and the next cycle will try again. The
         * component brings its own rail back down on this path. */
        ESP_LOGW(TAG, "panel init failed: %s - screen not updated", esp_err_to_name(err));
        return;
    }

    const int64_t started_us = esp_timer_get_time();

    /* The first frame of a boot is a full refresh: nothing is known about what
     * is on the glass, and a partial refresh on top of an unknown image keeps
     * whatever ghost was there. Afterwards the component decides, promoting to
     * a full refresh on its own schedule. */
    err = s_have_frame ? eink_render(&screen) : eink_render_full(&screen);
    const int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;

    if (err == ESP_OK) {
        s_shown = screen;
        s_have_frame = true;
        /* The duration is logged because it is the one cost of this feature
         * that is easy to forget: on the four-colour panel it is twenty
         * seconds of the measure task doing nothing else. */
        ESP_LOGI(TAG, "screen updated in %lld ms: soil %d %% (%s), vbat %d mV%s%s",
                 elapsed_ms, screen.soil_percent, level_name(screen.moisture),
                 screen.battery_mv, screen.charging ? ", charging" : "",
                 screen.battery_low ? ", battery low" : "");
    } else {
        ESP_LOGW(TAG, "refresh failed after %lld ms: %s", elapsed_ms, esp_err_to_name(err));
    }

    /* Unconditional: the rail comes down whether the refresh worked or not, and
     * the controller goes to sleep. The image stays on the glass by itself. */
    const esp_err_t power_err = eink_power_off();
    if (power_err != ESP_OK) {
        ESP_LOGE(TAG, "panel did not power down: %s", esp_err_to_name(power_err));
    }
}
