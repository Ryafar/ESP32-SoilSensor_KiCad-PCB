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

/* Screen layout, in pixels on the 200x200 glass. Collected here rather than
 * spread through the drawing code: this is the part that gets nudged when the
 * result is looked at on real hardware, and nudging it should not mean reading
 * the renderer. */
#define MARGIN 8

#define HEADER_Y 10
#define DROP_RADIUS 11
#define DROP_CX (MARGIN + DROP_RADIUS + 4)
/* The taper is two radii tall and ends at the disc centre, so the centre sits
 * two radii below the top of the icon and the icon is three radii tall. */
#define DROP_CY (HEADER_Y + 2 * DROP_RADIUS)

#define BATTERY_W 46
#define BATTERY_H 24
#define BATTERY_X (EINK_CANVAS_WIDTH - MARGIN - BATTERY_W - 5) // room for the nub
#define BATTERY_Y HEADER_Y
#define BATTERY_WALL 3
#define BATTERY_NUB_W 5
#define BATTERY_NUB_H 10

/* Sized so the widest case - "100%" - still clears the margins, since that is
 * the one that decides the digit width and every narrower reading is centred
 * inside it anyway. */
#define VALUE_Y 62
#define VALUE_DIGIT_W 40
#define VALUE_DIGIT_H 78
#define VALUE_STROKE 10
#define VALUE_GAP 9
#define PERCENT_W 28
#define PERCENT_H 38
#define PERCENT_STROKE 6
#define PERCENT_GAP 8

#define BAR_X MARGIN
#define BAR_Y 162
#define BAR_W (EINK_CANVAS_WIDTH - 2 * MARGIN)
#define BAR_H 26
#define BAR_WALL 3

/* Drawn around everything when the cell is nearly empty. Deliberately loud:
 * this is the one state where the reading on the screen matters less than the
 * fact that the device is about to shut itself off. */
#define ALERT_FRAME_THICKNESS 5

/* Coarse state-of-charge for the battery icon, linear between these two. It is
 * an indicator, not a fuel gauge - a real one needs the discharge curve and a
 * load model, and the icon is five pixels wide per 20 %. The low end is the
 * firmware's ship-mode threshold, so a visibly empty icon and a shutdown mean
 * the same thing. */
#define BATTERY_EMPTY_MV 3300
#define BATTERY_FULL_MV 4200

static epd_bus_handle_t s_bus;
static bool s_ready;
static unsigned s_frames_since_full;

static int battery_percent(int millivolts)
{
    if (millivolts <= BATTERY_EMPTY_MV) {
        return 0;
    }
    if (millivolts >= BATTERY_FULL_MV) {
        return 100;
    }
    return (millivolts - BATTERY_EMPTY_MV) * 100 / (BATTERY_FULL_MV - BATTERY_EMPTY_MV);
}

/* A water drop: a disc with a tapered tip, drawn as one span per row so it
 * needs no polygon fill.
 *
 * The taper runs all the way down to the centre of the disc, not to the top of
 * it. Stopping at the top leaves the widest part of the cone sitting on the
 * narrowest part of the sphere, and the icon reads as a triangle balanced on a
 * ball; carrying it to the centre makes the two shapes share a tangent and the
 * outline comes out as one teardrop. */
static void draw_drop(int cx, int cy, int radius, eink_color_t color)
{
    eink_canvas_disc(cx, cy, radius, color);

    const int tip_height = radius * 2;
    for (int row = 0; row < tip_height; row++) {
        const int half = (radius * row) / tip_height;
        eink_canvas_fill_rect(cx - half, cy - tip_height + row, 2 * half + 1, 1, color);
    }
}

static void draw_battery(int x, int y, int percent, bool charging, eink_color_t color)
{
    eink_canvas_rect(x, y, BATTERY_W, BATTERY_H, BATTERY_WALL, color);
    eink_canvas_fill_rect(x + BATTERY_W, y + (BATTERY_H - BATTERY_NUB_H) / 2, BATTERY_NUB_W,
                          BATTERY_NUB_H, color);

    /* The fill sits inside the wall with one pixel of clearance, so a full
     * battery still reads as a filled outline rather than a solid block. */
    const int inset = BATTERY_WALL + 1;
    const int track_w = BATTERY_W - 2 * inset;
    eink_canvas_fill_rect(x + inset, y + inset, track_w * percent / 100, BATTERY_H - 2 * inset,
                          color);

    if (charging) {
        /* A bolt across the icon. Drawn in the accent colour so it stays
         * visible over the fill on the four-colour panel; on the black/white
         * one the fill is what it is drawn over, which is why it gets a white
         * outline stroke first. */
        const int cx = x + BATTERY_W / 2;
        const int top = y + 2;
        const int bottom = y + BATTERY_H - 2;
        const int middle = y + BATTERY_H / 2;
        eink_canvas_line(cx + 6, top, cx - 3, middle, 7, EINK_WHITE);
        eink_canvas_line(cx - 3, middle, cx + 4, middle, 7, EINK_WHITE);
        eink_canvas_line(cx + 4, middle, cx - 5, bottom, 7, EINK_WHITE);
        eink_canvas_line(cx + 6, top, cx - 3, middle, 3, EINK_ACCENT);
        eink_canvas_line(cx - 3, middle, cx + 4, middle, 3, EINK_ACCENT);
        eink_canvas_line(cx + 4, middle, cx - 5, bottom, 3, EINK_ACCENT);
    }
}

static void draw_bar(int percent, eink_color_t color)
{
    eink_canvas_rect(BAR_X, BAR_Y, BAR_W, BAR_H, BAR_WALL, color);
    if (percent <= 0) {
        return;
    }

    const int inset = BAR_WALL + 2;
    const int track_w = BAR_W - 2 * inset;
    eink_canvas_fill_rect(BAR_X + inset, BAR_Y + inset, track_w * percent / 100,
                          BAR_H - 2 * inset, color);
}

/* The one place that turns a moisture level into ink. Yellow and red are only
 * real on the four-colour panel; the black/white one draws all three the same,
 * because the alternative there is dithering a digit, which costs legibility
 * and buys nothing. */
static eink_color_t moisture_color(eink_moisture_level_t level)
{
#if EINK_PANEL_JD79660_COLOR
    switch (level) {
    case EINK_MOISTURE_ALERT:
        return EINK_RED;
    case EINK_MOISTURE_WARN:
        return EINK_YELLOW;
    case EINK_MOISTURE_OK:
        break;
    }
    return EINK_BLACK;
#else
    (void)level;
    return EINK_BLACK;
#endif
}

static void draw_screen(const eink_screen_t *screen)
{
    eink_canvas_clear(EINK_WHITE);

    /* The drop stays black: it is the label for the reading, not part of it.
     * Colouring both the icon and the number would leave nothing to compare
     * the colour against. */
    draw_drop(DROP_CX, DROP_CY, DROP_RADIUS, EINK_BLACK);
    draw_battery(BATTERY_X, BATTERY_Y, battery_percent(screen->battery_mv), screen->charging,
                 screen->battery_low ? EINK_ACCENT : EINK_BLACK);

    /* The number and the percent sign are centred as one block, so 8 % and
     * 100 % both sit in the middle instead of the number wandering. */
    const int number_w = eink_canvas_number_width(screen->soil_percent, VALUE_DIGIT_W, VALUE_GAP);
    const int block_w = number_w + PERCENT_GAP + PERCENT_W;
    const int block_x = (EINK_CANVAS_WIDTH - block_w) / 2;

    /* Number and bar share the colour, so the reading says "water me" from
     * across the room without anyone reading the digits. */
    const eink_color_t value_color = moisture_color(screen->moisture);

    eink_canvas_number(block_x, VALUE_Y, VALUE_DIGIT_W, VALUE_DIGIT_H, VALUE_STROKE, VALUE_GAP,
                       screen->soil_percent, value_color);
    eink_canvas_percent(block_x + number_w + PERCENT_GAP, VALUE_Y, PERCENT_W, PERCENT_H,
                        PERCENT_STROKE, value_color);

    /* An empty outline for "no reading" rather than a bar at zero, which would
     * claim bone-dry soil the sensor never measured. */
    draw_bar(screen->soil_percent < 0 ? 0 : screen->soil_percent, value_color);

    if (screen->battery_low) {
        eink_canvas_rect(0, 0, EINK_CANVAS_WIDTH, EINK_CANVAS_HEIGHT, ALERT_FRAME_THICKNESS,
                         EINK_ACCENT);
    }
}

static esp_err_t push_frame(bool force_full)
{
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

esp_err_t eink_render(const eink_screen_t *screen)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(screen != NULL, ESP_ERR_INVALID_ARG, TAG, "screen is NULL");

    draw_screen(screen);
    return push_frame(false);
}

esp_err_t eink_render_full(const eink_screen_t *screen)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(screen != NULL, ESP_ERR_INVALID_ARG, TAG, "screen is NULL");

    draw_screen(screen);
    return push_frame(true);
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
    return push_frame(true);
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
