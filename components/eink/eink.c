#include "eink.h"

#include <limits.h>

#include "eink_canvas.h"
#include "eink_panel.h"
#include "epd_bus.h"
#include "esp_check.h"
#include "sdkconfig.h"

#if EINK_PANEL_JD79660_COLOR
#include "jd79660.h"
#else
#include "ssd1681.h"
#endif

static const char *TAG = "eink";

static epd_bus_handle_t s_bus;
static bool s_ready;
static unsigned s_frames_since_full;

esp_err_t eink_refresh(bool force_full)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    const uint8_t *frame = eink_canvas_frame();
    ESP_RETURN_ON_FALSE(frame != NULL, ESP_ERR_INVALID_STATE, TAG, "no framebuffer");

#if EINK_PANEL_JD79660_COLOR
    /* The four-colour panel has one refresh mode, so the distinction the
     * caller is drawing does not exist here. */
    (void)force_full;
    return jd79660_refresh(frame);
#else
    const bool full = force_full || CONFIG_EINK_FULL_REFRESH_INTERVAL == 0 ||
                      s_frames_since_full >= (unsigned)CONFIG_EINK_FULL_REFRESH_INTERVAL;
    if (full) {
        ESP_RETURN_ON_ERROR(ssd1681_refresh_full(frame), TAG, "full refresh failed");
        s_frames_since_full = 0;
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ssd1681_refresh_partial(frame), TAG, "partial refresh failed");
    s_frames_since_full++;
    return ESP_OK;
#endif
}

esp_err_t eink_init(const eink_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(!s_ready, ESP_ERR_INVALID_STATE, TAG, "already initialized");

    const epd_bus_config_t bus_config = {
        .spi_host = config->spi_host,
        .pin_clk = config->pin_clk,
        .pin_mosi = config->pin_mosi,
        .pin_cs = config->pin_cs,
        .pin_dc = config->pin_dc,
        .pin_rst = config->pin_rst,
        .pin_busy = config->pin_busy,
        .pin_power = config->pin_power,
        .power_active_low = config->power_active_low,
        .clock_hz = CONFIG_EINK_SPI_CLOCK_HZ,
#if EINK_PANEL_JD79660_COLOR
        .busy_active_level = 0, // JD79660 holds BUSY low while it works
#else
        .busy_active_level = 1, // SSD1681 holds BUSY high while it works
#endif
    };

    if (s_bus == NULL) {
        ESP_RETURN_ON_ERROR(epd_bus_new(&bus_config, &s_bus), TAG, "bus setup failed");
    }

    /* The canvas outlives a power cycle of the panel - re-init after wake
     * should not have to redraw from scratch - so it is only allocated once. */
    if (eink_canvas_frame() == NULL) {
        ESP_RETURN_ON_ERROR(eink_canvas_init(), TAG, "framebuffer allocation failed");
    }

#if EINK_PANEL_JD79660_COLOR
    const esp_err_t panel_err = jd79660_init(s_bus);
#else
    const esp_err_t panel_err = ssd1681_init(s_bus);
#endif
    if (panel_err != ESP_OK) {
        /* The controller's init powered the rail before it failed, and the
         * caller cannot take it down - eink_power_off() refuses while s_ready
         * is false. So it happens here: any function that brings a rail up
         * takes it down on every exit path, error paths included. A panel left
         * powered outlives the cell. */
        const esp_err_t power_err = epd_bus_power_off(s_bus);
        if (power_err != ESP_OK) {
            ESP_LOGE(TAG, "panel init failed AND the rail did not come down: %s",
                     esp_err_to_name(power_err));
        }
        ESP_RETURN_ON_ERROR(panel_err, TAG, "panel init failed");
    }

    /* Nothing is known about what is on the glass after a reset, so the next
     * refresh has to be a full one whatever the caller asks for. */
    s_frames_since_full = UINT_MAX;
    s_ready = true;
    return ESP_OK;
}

/* Big enough to count across the glass at arm's length, and a divisor of 200 so
 * the pattern ends flush with the edge instead of on a partial square. */
#define PATTERN_CHECKER_PX 25

static void draw_pattern(eink_pattern_t pattern)
{
    switch (pattern) {
    case EINK_PATTERN_BLACK:
        eink_canvas_clear(EINK_BLACK);
        break;

    case EINK_PATTERN_QUADRANT:
        /* The square says which way round the panel is mounted; the frame says
         * whether the whole 200x200 window is addressed and nothing is shifted
         * off an edge. Two questions, one refresh. */
        eink_canvas_clear(EINK_WHITE);
        eink_canvas_fill_rect(0, 0, EINK_CANVAS_WIDTH / 2, EINK_CANVAS_HEIGHT / 2, EINK_BLACK);
        eink_canvas_rect(0, 0, EINK_CANVAS_WIDTH, EINK_CANVAS_HEIGHT, 1, EINK_BLACK);
        break;

    case EINK_PATTERN_CHECKER:
        eink_canvas_clear(EINK_WHITE);
        for (int y = 0; y < EINK_CANVAS_HEIGHT; y += PATTERN_CHECKER_PX) {
            for (int x = 0; x < EINK_CANVAS_WIDTH; x += PATTERN_CHECKER_PX) {
                if (((x / PATTERN_CHECKER_PX) + (y / PATTERN_CHECKER_PX)) % 2 == 0) {
                    eink_canvas_fill_rect(x, y, PATTERN_CHECKER_PX, PATTERN_CHECKER_PX,
                                          EINK_BLACK);
                }
            }
        }
        break;

    case EINK_PATTERN_COLORS: {
        /* Only the four-colour panel can print this. On the black/white one
         * every band except the first lands as black, which is correct but
         * says nothing - hence the test only asks for it on the colour panel. */
        const eink_color_t bands[] = {EINK_WHITE, EINK_BLACK, EINK_YELLOW, EINK_RED};
        const int count = (int)(sizeof(bands) / sizeof(bands[0]));
        const int height = EINK_CANVAS_HEIGHT / count;
        for (int i = 0; i < count; i++) {
            eink_canvas_fill_rect(0, i * height, EINK_CANVAS_WIDTH, height, bands[i]);
        }
        break;
    }

    case EINK_PATTERN_WHITE:
    default:
        eink_canvas_clear(EINK_WHITE);
        break;
    }
}

esp_err_t eink_show_pattern(eink_pattern_t pattern)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    draw_pattern(pattern);
    return eink_refresh(true);
}

bool eink_is_busy(void)
{
    return epd_bus_is_busy(s_bus);
}

esp_err_t eink_power_off(void)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    /* Deep sleep first, then the rail. If the sleep command fails the rail
     * still has to come down - a panel left powered outlives the cell - so its
     * result is logged rather than returned. */
#if EINK_PANEL_JD79660_COLOR
    const esp_err_t sleep_err = jd79660_sleep();
#else
    const esp_err_t sleep_err = ssd1681_sleep();
#endif
    if (sleep_err != ESP_OK) {
        ESP_LOGW(TAG, "controller did not acknowledge deep sleep: %s", esp_err_to_name(sleep_err));
    }

    const esp_err_t power_err = epd_bus_power_off(s_bus);
    s_ready = false;
    ESP_RETURN_ON_ERROR(power_err, TAG, "panel rail did not come down");
    return sleep_err;
}
