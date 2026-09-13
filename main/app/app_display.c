#include "app_display.h"

#include "app_screen.h"
#include "config.h"
#include "eink.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "app_display";

/* The battery is drawn as a coarse icon, so comparing raw millivolts would
 * report a change on every reading and refresh the panel for a picture that is
 * pixel for pixel identical. 50 mV is finer than the icon can show and coarse
 * enough that ADC noise does not trigger a 21-second refresh. */
#define BATTERY_COMPARE_STEP_MV 50

/* What the glass is currently showing, in the terms that decide the picture. */
typedef struct {
    int soil_percent;
    int soil_level;     // which colour band the percentage falls in
    int battery_step;
    bool charging;
    bool battery_low;
} shown_state_t;

/* Both survive deep sleep, and that is the difference between a device that
 * refreshes when the reading changes and one that spends 21 seconds redrawing
 * the same picture on every single wake. RAM is cleared between cycles; RTC
 * memory is not. */
static RTC_DATA_ATTR bool s_have_frame;
static RTC_DATA_ATTR shown_state_t s_shown;

/* The colour band, so that crossing a threshold redraws even when the
 * percentage moved by less than the step below. Same thresholds app_screen
 * draws with; kept as a number rather than the component's enum so this file
 * does not have to care what the panel can show. */
static int soil_level_of(int soil_percent)
{
    if (soil_percent < 0) {
        return 0;
    }
    if (soil_percent <= SOIL_ALERT_PERCENT) {
        return 1;
    }
    if (soil_percent <= SOIL_WARN_PERCENT) {
        return 2;
    }
    return 3;
}

static shown_state_t state_of(const app_sample_t *sample, bool battery_low)
{
    return (shown_state_t){
        .soil_percent = sample->soil_percent,
        .soil_level = soil_level_of(sample->soil_percent),
        .battery_step = sample->battery_mv / BATTERY_COMPARE_STEP_MV,
        .charging = sample->charging,
        .battery_low = battery_low,
    };
}

static bool differs(const shown_state_t *a, const shown_state_t *b)
{
    /* A reading appearing or disappearing is always worth a redraw: dashes and
     * a number are not a small difference. */
    if ((a->soil_percent < 0) != (b->soil_percent < 0)) {
        return true;
    }

    const int soil_moved = a->soil_percent > b->soil_percent
                               ? a->soil_percent - b->soil_percent
                               : b->soil_percent - a->soil_percent;

    return soil_moved >= SOIL_DISPLAY_STEP_PERCENT || a->soil_level != b->soil_level ||
           a->battery_step != b->battery_step || a->charging != b->charging ||
           a->battery_low != b->battery_low;
}

void app_display_update(const app_sample_t *sample, bool battery_low)
{
    if (sample == NULL) {
        return;
    }

    const shown_state_t state = state_of(sample, battery_low);
    if (s_have_frame && !differs(&state, &s_shown)) {
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

    app_screen_draw(sample, battery_low);

    /* The first frame of a boot gets the full waveform: nothing is known about
     * what is on the glass, and a partial refresh on top of an unknown image
     * keeps whatever ghost was there. After that the component decides,
     * promoting to a full refresh on its own schedule. */
    err = eink_refresh(!s_have_frame);
    const int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;

    if (err == ESP_OK) {
        s_shown = state;
        s_have_frame = true;
        /* The duration is logged because it is the one cost of this feature
         * that is easy to forget: on the four-colour panel it is twenty
         * seconds of the measure task doing nothing else. */
        ESP_LOGI(TAG, "screen updated in %lld ms: soil %d %%, vbat %d mV%s%s", elapsed_ms,
                 sample->soil_percent, sample->battery_mv, sample->charging ? ", charging" : "",
                 battery_low ? ", battery low" : "");
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
