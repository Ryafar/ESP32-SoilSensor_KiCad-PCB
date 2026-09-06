#include "eink_canvas.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"

static const char *TAG = "eink_canvas";

/* Seven-segment layout. One bit per segment, in the order they are drawn. */
#define SEG_TOP 0x01
#define SEG_TOP_LEFT 0x02
#define SEG_TOP_RIGHT 0x04
#define SEG_MIDDLE 0x08
#define SEG_BOTTOM_LEFT 0x10
#define SEG_BOTTOM_RIGHT 0x20
#define SEG_BOTTOM 0x40

static const uint8_t k_digit_segments[10] = {
    SEG_TOP | SEG_TOP_LEFT | SEG_TOP_RIGHT | SEG_BOTTOM_LEFT | SEG_BOTTOM_RIGHT | SEG_BOTTOM, // 0
    SEG_TOP_RIGHT | SEG_BOTTOM_RIGHT,                                                         // 1
    SEG_TOP | SEG_TOP_RIGHT | SEG_MIDDLE | SEG_BOTTOM_LEFT | SEG_BOTTOM,                      // 2
    SEG_TOP | SEG_TOP_RIGHT | SEG_MIDDLE | SEG_BOTTOM_RIGHT | SEG_BOTTOM,                     // 3
    SEG_TOP_LEFT | SEG_TOP_RIGHT | SEG_MIDDLE | SEG_BOTTOM_RIGHT,                             // 4
    SEG_TOP | SEG_TOP_LEFT | SEG_MIDDLE | SEG_BOTTOM_RIGHT | SEG_BOTTOM,                      // 5
    SEG_TOP | SEG_TOP_LEFT | SEG_MIDDLE | SEG_BOTTOM_LEFT | SEG_BOTTOM_RIGHT | SEG_BOTTOM,    // 6
    SEG_TOP | SEG_TOP_RIGHT | SEG_BOTTOM_RIGHT,                                               // 7
    SEG_TOP | SEG_TOP_LEFT | SEG_TOP_RIGHT | SEG_MIDDLE | SEG_BOTTOM_LEFT | SEG_BOTTOM_RIGHT |
        SEG_BOTTOM,                                                                           // 8
    SEG_TOP | SEG_TOP_LEFT | SEG_TOP_RIGHT | SEG_MIDDLE | SEG_BOTTOM_RIGHT | SEG_BOTTOM,      // 9
};

/* How many dashes stand in for a value that could not be measured. Two, so
 * "--" sits under the same layout as a two-digit percentage. */
#define UNKNOWN_DIGITS 2

static uint8_t *s_frame;

static inline bool in_bounds(int x, int y)
{
    return x >= 0 && x < EINK_CANVAS_WIDTH && y >= 0 && y < EINK_CANVAS_HEIGHT;
}

/* The one place the two pixel formats differ. */
static inline void write_pixel(int x, int y, eink_color_t color)
{
#if EINK_CANVAS_BITS_PER_PIXEL == 1
    /* A set bit is a white pixel on the SSD1681, so every colour that is not
     * white becomes black - an accent the panel cannot print is still meant to
     * be visible. */
    uint8_t *byte = &s_frame[y * EINK_CANVAS_ROW_BYTES + (x >> 3)];
    const uint8_t mask = (uint8_t)(0x80u >> (x & 7));
    if (color == EINK_WHITE) {
        *byte |= mask;
    } else {
        *byte &= (uint8_t)~mask;
    }
#else
    /* Four pixels per byte, leftmost in bits 7:6. */
    uint8_t *byte = &s_frame[y * EINK_CANVAS_ROW_BYTES + (x >> 2)];
    const int shift = 6 - 2 * (x & 3);
    *byte = (uint8_t)((*byte & ~(0x03u << shift)) | ((color & 0x03u) << shift));
#endif
}

/* A byte of identical pixels, for clearing. */
static inline uint8_t solid_byte(eink_color_t color)
{
#if EINK_CANVAS_BITS_PER_PIXEL == 1
    return color == EINK_WHITE ? 0xFF : 0x00;
#else
    return (uint8_t)((color & 0x03u) * 0x55u);
#endif
}

esp_err_t eink_canvas_init(void)
{
    ESP_RETURN_ON_FALSE(s_frame == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialized");

    s_frame = heap_caps_malloc(EINK_CANVAS_BYTES, MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_frame != NULL, ESP_ERR_NO_MEM, TAG, "no memory for a %d byte frame",
                        EINK_CANVAS_BYTES);

    eink_canvas_clear(EINK_WHITE);
    return ESP_OK;
}

void eink_canvas_deinit(void)
{
    free(s_frame);
    s_frame = NULL;
}

const uint8_t *eink_canvas_frame(void)
{
    return s_frame;
}

void eink_canvas_clear(eink_color_t color)
{
    if (s_frame == NULL) {
        return;
    }
    memset(s_frame, solid_byte(color), EINK_CANVAS_BYTES);
}

void eink_canvas_pixel(int x, int y, eink_color_t color)
{
    if (s_frame == NULL || !in_bounds(x, y)) {
        return;
    }
    write_pixel(x, y, color);
}

void eink_canvas_fill_rect(int x, int y, int w, int h, eink_color_t color)
{
    if (s_frame == NULL || w <= 0 || h <= 0) {
        return;
    }

    /* Clip once here rather than testing every pixel. */
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > EINK_CANVAS_WIDTH ? EINK_CANVAS_WIDTH : x + w;
    int y1 = y + h > EINK_CANVAS_HEIGHT ? EINK_CANVAS_HEIGHT : y + h;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    for (int row = y0; row < y1; row++) {
        for (int col = x0; col < x1; col++) {
            write_pixel(col, row, color);
        }
    }
}

void eink_canvas_rect(int x, int y, int w, int h, int thickness, eink_color_t color)
{
    if (thickness <= 0 || w <= 0 || h <= 0) {
        return;
    }
    if (thickness * 2 >= w || thickness * 2 >= h) {
        eink_canvas_fill_rect(x, y, w, h, color);
        return;
    }

    eink_canvas_fill_rect(x, y, w, thickness, color);
    eink_canvas_fill_rect(x, y + h - thickness, w, thickness, color);
    eink_canvas_fill_rect(x, y + thickness, thickness, h - 2 * thickness, color);
    eink_canvas_fill_rect(x + w - thickness, y + thickness, thickness, h - 2 * thickness, color);
}

void eink_canvas_line(int x0, int y0, int x1, int y1, int thickness, eink_color_t color)
{
    if (thickness <= 0) {
        return;
    }

    const int dx = abs(x1 - x0);
    const int dy = -abs(y1 - y0);
    const int step_x = x0 < x1 ? 1 : -1;
    const int step_y = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    const int offset = thickness / 2;

    while (1) {
        eink_canvas_fill_rect(x0 - offset, y0 - offset, thickness, thickness, color);
        if (x0 == x1 && y0 == y1) {
            return;
        }
        const int error2 = 2 * error;
        if (error2 >= dy) {
            error += dy;
            x0 += step_x;
        }
        if (error2 <= dx) {
            error += dx;
            y0 += step_y;
        }
    }
}

void eink_canvas_disc(int cx, int cy, int radius, eink_color_t color)
{
    if (radius <= 0) {
        return;
    }

    /* One span per row, found by walking the half-width in rather than by a
     * square root - no floating point, and the radii here are small. */
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = radius;
        while (dx > 0 && dx * dx + dy * dy > radius * radius) {
            dx--;
        }
        eink_canvas_fill_rect(cx - dx, cy + dy, 2 * dx + 1, 1, color);
    }
}

void eink_canvas_digit(int x, int y, int w, int h, int stroke, int digit, eink_color_t color)
{
    if (w <= 2 * stroke || h <= 3 * stroke || stroke <= 0) {
        return;
    }

    /* Vertical segments run between the horizontal ones, so their length falls
     * out of where the middle bar sits. */
    const int half = (h - stroke) / 2;
    const int bar_len = half - stroke;

    const uint8_t segments = (digit >= 0 && digit <= 9) ? k_digit_segments[digit] : SEG_MIDDLE;

    /* The horizontal bars run the full width rather than being inset by the
     * stroke the way a real LCD segment is. At this stroke weight - a quarter
     * of the digit width, because the panel is 200 px across and the number has
     * to be readable from across a room - an inset leaves a square hole at each
     * corner, and the digit falls apart into loose blocks. Full-width bars turn
     * those corners into solid joints. */
    if (segments & SEG_TOP) {
        eink_canvas_fill_rect(x, y, w, stroke, color);
    }
    if (segments & SEG_MIDDLE) {
        eink_canvas_fill_rect(x, y + half, w, stroke, color);
    }
    if (segments & SEG_BOTTOM) {
        eink_canvas_fill_rect(x, y + h - stroke, w, stroke, color);
    }
    if (segments & SEG_TOP_LEFT) {
        eink_canvas_fill_rect(x, y + stroke, stroke, bar_len, color);
    }
    if (segments & SEG_TOP_RIGHT) {
        eink_canvas_fill_rect(x + w - stroke, y + stroke, stroke, bar_len, color);
    }
    if (segments & SEG_BOTTOM_LEFT) {
        eink_canvas_fill_rect(x, y + half + stroke, stroke, bar_len, color);
    }
    if (segments & SEG_BOTTOM_RIGHT) {
        eink_canvas_fill_rect(x + w - stroke, y + half + stroke, stroke, bar_len, color);
    }
}

static int digit_count(int value)
{
    if (value < 0) {
        return UNKNOWN_DIGITS;
    }
    int digits = 1;
    while (value >= 10) {
        value /= 10;
        digits++;
    }
    return digits;
}

int eink_canvas_number_width(int value, int digit_w, int gap)
{
    const int digits = digit_count(value);
    return digits * digit_w + (digits - 1) * gap;
}

void eink_canvas_number(int x, int y, int digit_w, int digit_h, int stroke, int gap, int value,
                        eink_color_t color)
{
    const int digits = digit_count(value);

    /* Rendered right to left so each digit can be peeled off the value, then
     * placed from its own left edge. */
    for (int position = digits - 1; position >= 0; position--) {
        const int digit = value < 0 ? -1 : value % 10;
        eink_canvas_digit(x + position * (digit_w + gap), y, digit_w, digit_h, stroke, digit,
                          color);
        if (value >= 0) {
            value /= 10;
        }
    }
}

void eink_canvas_percent(int x, int y, int w, int h, int stroke, eink_color_t color)
{
    /* Two solid squares and a slash. Not rings: at this size the hole in a ring
     * would be a pixel or two across, which the panel renders as a smudge
     * rather than a hole, so the square is both simpler and what actually shows
     * up on the glass. */
    const int ring = w / 3;
    if (ring <= 0) {
        return;
    }

    eink_canvas_fill_rect(x, y, ring, ring, color);
    eink_canvas_fill_rect(x + w - ring, y + h - ring, ring, ring, color);
    eink_canvas_line(x + stroke, y + h - stroke, x + w - stroke, y + stroke, stroke, color);
}
