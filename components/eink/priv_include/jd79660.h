/* Fitipower JD79660 - 200x200 four-colour e-paper controller.
 *
 * The controller on the WeAct Studio 1.54" 4C module, panel GDEM0154F51H. The
 * board is not populated with this variant today; the driver exists so the
 * swap is a Kconfig choice rather than a rewrite.
 *
 * Register flow taken from GxEPD2's GxEPD2_154c_GDEM0154F51H, which follows
 * Good Display's own demo for the panel. WeAct ships only the datasheet PDF for
 * this variant, no reference code.
 *
 * Chip driver only: it takes a packed 2 bpp frame and puts it on the glass. It
 * owns no framebuffer and decides nothing about refresh policy. The transport
 * is borrowed through an epd_bus handle the caller creates.
 *
 * Frame format: 10000 bytes, row-major, 50 bytes per 200-pixel row, four pixels
 * per byte with the leftmost in bits 7:6. Colour codes are the controller's own
 * (see jd79660_color_t), so no conversion happens in this driver.
 *
 * Two differences from the black/white part that matter to callers: BUSY is
 * asserted LOW here, and a refresh takes on the order of 20 seconds rather than
 * 2.5. On a coin-cell-budget device that second one is a design constraint, not
 * a detail - see docs/hardware/hardware-info.md.
 *
 * Not thread-safe, and single-instance: there is one panel on the board.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "epd_bus.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JD79660_WIDTH 200
#define JD79660_HEIGHT 200
#define JD79660_FRAME_BYTES ((JD79660_WIDTH / 4) * JD79660_HEIGHT) // 10000

/* The two-bit codes the controller expects per pixel. */
typedef enum {
    JD79660_BLACK = 0x00,
    JD79660_WHITE = 0x01,
    JD79660_YELLOW = 0x02,
    JD79660_RED = 0x03,
} jd79660_color_t;

/* A byte of four identical pixels, for solid fills. */
#define JD79660_SOLID_BYTE(color) ((uint8_t)((color) * 0x55))

/* Powers the rail, resets the controller and runs the init sequence. This part
 * also switches its own power supply on at the end of init, so the panel is
 * live when this returns. */
esp_err_t jd79660_init(epd_bus_handle_t bus);

/* Displays a frame. There is no cheap update on this controller: every refresh
 * is a full one, drives all four colours, and blocks for ~20 s. */
esp_err_t jd79660_refresh(const uint8_t *frame);

/* Paints the whole panel one colour without needing a frame in memory. */
esp_err_t jd79660_fill_screen(jd79660_color_t color);

/* Turns the panel supply off and puts the controller into deep sleep. Only a
 * hardware reset brings it back, so jd79660_init() must run again first. */
esp_err_t jd79660_sleep(void);

#ifdef __cplusplus
}
#endif
