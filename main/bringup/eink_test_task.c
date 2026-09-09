#include "eink_test_task.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "eink.h"
#include "eink_panel.h"

static const char *TAG = "eink_test";

/* Long enough to look at the panel and compare it against the log line. The
 * four-colour panel earns a longer look: its refresh alone is ~21 s, so a short
 * hold makes the whole run feel like it is racing past. */
#if EINK_PANEL_JD79660_COLOR
#define SCREEN_HOLD_MS 8000
#else
#define SCREEN_HOLD_MS 4000
#endif

/* Stage one: patterns, before any layout. If one of these is wrong the problem
 * is the bus, the controller or the panel - the layout code cannot be blamed
 * for a screen that will not go black. Each line says what should be on the
 * glass, so the log can be read next to the panel. */
static const struct {
    const char *description;
    eink_pattern_t pattern;
} k_patterns[] = {
    {"all white - the glass should clear completely, no ghosting left over",
     EINK_PATTERN_WHITE},
    {"all black - fully dark. Grey or patchy means the waveform or the supply, "
     "not the drawing code",
     EINK_PATTERN_BLACK},
    {"black square in ONE quarter, thin frame around the whole screen. Which "
     "corner the square sits in tells you the orientation; a missing frame edge "
     "means the window is offset",
     EINK_PATTERN_QUADRANT},
    {"25 px checkerboard, 8x8 squares, flush with all four edges. Diagonal "
     "stripes instead of squares means the row stride is wrong",
     EINK_PATTERN_CHECKER},
#if EINK_PANEL_JD79660_COLOR
    {"four horizontal bands, top to bottom: white, black, yellow, red. This is "
     "the only step that exercises the colour planes at all",
     EINK_PATTERN_COLORS},
#endif
};

/* Each step names what should be on the glass, so the log and the panel can be
 * read side by side. The values are made up - nothing is measured yet. */
static const struct {
    const char *description;
    eink_screen_t screen;
    bool full_refresh;
} k_sequence[] = {
    {
        "no reading yet: two dashes, empty bar, half-full battery",
        {.soil_percent = -1, .battery_mv = 3800, .charging = false, .battery_low = false},
        true,
    },
    {
        "dry soil: 7 %, bar barely started",
        {.soil_percent = 7, .battery_mv = 3800, .charging = false, .battery_low = false},
        false,
    },
    {
        "damp soil: 42 %, bar under half",
        {.soil_percent = 42, .battery_mv = 3950, .charging = false, .battery_low = false},
        false,
    },
    {
        "wet soil on USB: 100 %, full bar, charge bolt in the battery",
        {.soil_percent = 100, .battery_mv = 4150, .charging = true, .battery_low = false},
        false,
    },
    {
        "low battery: empty battery icon and a frame around the screen",
        {.soil_percent = 42, .battery_mv = 3320, .charging = false, .battery_low = true},
        true,
    },
};

void eink_test_task(void *arg)
{
    (void)arg;

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
        /* A timeout here means BUSY never released, which looks the same
         * whether the panel is missing, BUSY is on another pin, or the rail
         * never came up. PAPER_POWER polarity is no longer a suspect - it is
         * confirmed active low in config.h. */
        ESP_LOGE(TAG, "eink_init failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "check: is the FPC seated, is the panel picked in "
                      "eink_panel.h the one actually fitted, and is BUSY really "
                      "on GPIO%d", PIN_EPD_BUSY);
        vTaskDelete(NULL);
        return;
    }

#if EINK_PANEL_JD79660_COLOR
    ESP_LOGI(TAG, "four-colour panel: every refresh is a full one (~21 s) - the "
                  "partial/full labels below do not apply, the controller has "
                  "only one mode. Nine screens, so budget about four minutes");
#else
    ESP_LOGI(TAG, "black/white panel: ~2.5 s full refresh, ~0.5 s partial");
#endif

    for (size_t i = 0; i < sizeof(k_patterns) / sizeof(k_patterns[0]); i++) {
        ESP_LOGI(TAG, "pattern %u/%u: %s", (unsigned)(i + 1),
                 (unsigned)(sizeof(k_patterns) / sizeof(k_patterns[0])),
                 k_patterns[i].description);

        err = eink_show_pattern(k_patterns[i].pattern);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "pattern failed: %s - stopping here, the layout "
                          "screens would only add noise", esp_err_to_name(err));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(SCREEN_HOLD_MS));
    }

    /* Stage two: the real layout. Only meaningful once the patterns above came
     * out right. */
    for (size_t i = 0; err == ESP_OK && i < sizeof(k_sequence) / sizeof(k_sequence[0]); i++) {
        ESP_LOGI(TAG, "screen %u/%u (%s refresh): %s", (unsigned)(i + 1),
                 (unsigned)(sizeof(k_sequence) / sizeof(k_sequence[0])),
                 k_sequence[i].full_refresh ? "full" : "partial", k_sequence[i].description);

        err = k_sequence[i].full_refresh ? eink_render_full(&k_sequence[i].screen)
                                         : eink_render(&k_sequence[i].screen);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "render failed: %s", esp_err_to_name(err));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(SCREEN_HOLD_MS));
    }

    /* The last image stays on the glass with the rail down - that is the whole
     * point of e-paper, and it is also the state the device sleeps in. */
    err = eink_power_off();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "eink_power_off failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "panel asleep and rail down, image should still be readable");
    }

    vTaskDelete(NULL);
}
