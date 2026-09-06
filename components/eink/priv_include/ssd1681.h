/* Solomon Systech SSD1681 - 200x200 black/white e-paper controller.
 *
 * The controller on the WeAct Studio 1.54" black/white module, panel
 * GDEH0154D67 (the module ships it under the vendor name YRD0150BBS810F0-M7).
 * Register flow taken from WeAct's own reference code
 * (WeActStudio.EpaperModule, Doc/1.54 Inch Black&Write) cross-checked against
 * GxEPD2's GxEPD2_154_D67, which is the more widely exercised of the two.
 *
 * Chip driver only: it takes a packed 1 bpp frame and puts it on the glass. It
 * owns no framebuffer, draws nothing and decides nothing about when a refresh
 * is worth its energy. The transport - SPI, D/C, RST, BUSY and the panel rail -
 * is borrowed through an epd_bus handle the caller creates.
 *
 * Frame format: 5000 bytes, row-major, 25 bytes per 200-pixel row, MSB of each
 * byte is the leftmost pixel, and a set bit is white. That is the controller's
 * own layout, so no conversion happens anywhere in this driver.
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

#define SSD1681_WIDTH 200
#define SSD1681_HEIGHT 200
#define SSD1681_FRAME_BYTES ((SSD1681_WIDTH / 8) * SSD1681_HEIGHT) // 5000

/* Powers the rail, resets the controller and runs the init sequence. The panel
 * shows whatever it was last refreshed with until a refresh call. */
esp_err_t ssd1681_init(epd_bus_handle_t bus);

/* Full refresh: every pixel is driven through the complete waveform. Slow
 * (~2.5 s) and it flashes, but it is the only thing that clears ghosting.
 *
 * Both RAM banks are written with the frame. The new-image bank is what gets
 * displayed; the old-image bank has to match afterwards or the next partial
 * refresh computes its difference against a stale image. */
esp_err_t ssd1681_refresh_full(const uint8_t *frame);

/* Partial refresh: only pixels that changed since the last frame are driven,
 * which takes ~0.5 s and does not flash. Ghosting accumulates, so the caller
 * has to fall back to a full refresh every so often.
 *
 * Requires a preceding full refresh - without one, the old-image bank holds
 * whatever survived reset and the difference is meaningless. */
esp_err_t ssd1681_refresh_partial(const uint8_t *frame);

/* Paints the whole panel one colour without needing a frame in memory. Used by
 * bring-up, and by anything that wants a known state before it draws. */
esp_err_t ssd1681_fill_screen(bool white);

/* Deep sleep. Draws ~1 uA and keeps the image; only a hardware reset brings the
 * controller back, so ssd1681_init() must run again before the next refresh. */
esp_err_t ssd1681_sleep(void);

#ifdef __cplusplus
}
#endif
