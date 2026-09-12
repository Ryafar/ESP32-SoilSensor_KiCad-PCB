/* Framebuffer and drawing primitives for the 200x200 panel.
 *
 * Panel-agnostic on purpose: the pixel format is chosen at build time by the
 * #define in eink_panel.h, so the same drawing code produces a 1 bpp frame for the
 * SSD1681 and a 2 bpp frame for the JD79660 with no conversion pass and no
 * second buffer. The colour codes are the JD79660's own, which makes the
 * four-colour packing a plain store; on the black/white panel every colour that
 * is not EINK_WHITE lands as black, which is the right answer for an accent
 * colour on a panel that cannot show it.
 *
 * There is no font here, and that is a limit worth knowing before reusing this:
 * it draws numbers, bars and shapes. Seven-segment digits built from rectangles
 * cost a few hundred bytes of code against a few kilobytes for a bitmap font,
 * and they are far more legible at the sizes a 200x200 panel needs - but a
 * project that has to show words needs a font added here.
 *
 * Coordinates are pixels from the top-left. Every primitive clips; drawing
 * off-canvas is a no-op, not an error, because the alternative is that a
 * one-pixel layout mistake becomes a crash on a device in a plant pot.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "eink_panel.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EINK_CANVAS_WIDTH 200
#define EINK_CANVAS_HEIGHT 200

#if EINK_PANEL_JD79660_COLOR
#define EINK_CANVAS_BITS_PER_PIXEL 2
#else
#define EINK_CANVAS_BITS_PER_PIXEL 1
#endif

#define EINK_CANVAS_ROW_BYTES ((EINK_CANVAS_WIDTH * EINK_CANVAS_BITS_PER_PIXEL) / 8)
#define EINK_CANVAS_BYTES (EINK_CANVAS_ROW_BYTES * EINK_CANVAS_HEIGHT)

/* Values are the JD79660's two-bit colour codes. On the black/white panel only
 * EINK_WHITE is white. */
typedef enum {
    EINK_BLACK = 0,
    EINK_WHITE = 1,
    EINK_YELLOW = 2,
    EINK_RED = 3,
} eink_color_t;

/* The colour used for anything that should stand out - a low battery, a charge
 * indicator. Red on the four-colour panel, black on the other one, which is as
 * much emphasis as it has to give. */
#if EINK_PANEL_JD79660_COLOR
#define EINK_ACCENT EINK_RED
#else
#define EINK_ACCENT EINK_BLACK
#endif

/* Allocates the framebuffer and clears it to white. */
esp_err_t eink_canvas_init(void);
void eink_canvas_deinit(void);

/* The packed frame, in the format the selected controller expects.
 * EINK_CANVAS_BYTES long, NULL before init. */
const uint8_t *eink_canvas_frame(void);

void eink_canvas_clear(eink_color_t color);
void eink_canvas_pixel(int x, int y, eink_color_t color);
void eink_canvas_fill_rect(int x, int y, int w, int h, eink_color_t color);

/* Outline drawn inside the given box, so the box is the outer edge. */
void eink_canvas_rect(int x, int y, int w, int h, int thickness, eink_color_t color);

/* Bresenham with a square brush, used for the diagonals no rectangle can
 * make - the percent sign and the charge bolt. */
void eink_canvas_line(int x0, int y0, int x1, int y1, int thickness, eink_color_t color);

void eink_canvas_disc(int cx, int cy, int radius, eink_color_t color);

/* Seven-segment digit filling the given box. digit must be 0-9; anything else
 * draws the middle bar alone, which is how an unknown value is shown. */
void eink_canvas_digit(int x, int y, int w, int h, int stroke, int digit, eink_color_t color);

/* Width a value would occupy, so callers can centre before drawing. */
int eink_canvas_number_width(int value, int digit_w, int gap);

/* Draws value left-aligned at x. Negative values draw as dashes, one per digit
 * of the widest expected value, because "no reading" is not the same as zero. */
void eink_canvas_number(int x, int y, int digit_w, int digit_h, int stroke, int gap, int value,
                        eink_color_t color);

/* Percent sign scaled to the same box as a digit. */
void eink_canvas_percent(int x, int y, int w, int h, int stroke, eink_color_t color);

#ifdef __cplusplus
}
#endif
