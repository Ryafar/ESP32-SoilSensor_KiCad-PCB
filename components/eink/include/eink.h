/* WeAct Studio 1.54" e-paper module, 200x200 - panel, transport and rail.
 *
 * Reusable as-is: this component knows about a panel, not about a device. It
 * has no idea what is being displayed, holds no layout and no thresholds, and
 * takes every board-specific value - pins, SPI host, rail polarity - through
 * eink_init(). Drop the directory into another project, point the config at
 * that board's pins, and it works.
 *
 * Two headers are public:
 *
 *   eink.h         this one: lifecycle, refresh, panel diagnostics.
 *   eink_canvas.h  the framebuffer and the drawing primitives.
 *
 * The split in use is: draw with the canvas, then call eink_refresh(). What the
 * picture means is the caller's business; this file only gets it onto glass.
 *
 * Inside, the component splits into four files, each named for what it is:
 *
 *   eink.c         lifecycle, refresh policy, bring-up patterns. This header.
 *   eink_canvas.c  drawing primitives and the pixel packing.
 *   epd_bus.c      SPI, D/C, RST, BUSY and the rail - the half both
 *                  controllers share.
 *   jd79660.c      the four-colour controller, register for register.
 *   ssd1681.c      the black/white one. Only one of the two is compiled.
 *
 * The controller files are named after their chips for the same reason a
 * charger driver is called bq25186 rather than "charger": the name says which
 * datasheet the register writes came from. eink.c itself contains no controller
 * registers at all.
 *
 * Two panels are supported, picked by the #define in eink_panel.h:
 *
 *   JD79660   WeAct Studio 1.54" four colour, panel GDEM0154F51H. Fitted
 *             today. 200x200, black/white/red/yellow, ~21 s per refresh and no
 *             partial mode - every update is a full one.
 *   SSD1681   WeAct Studio 1.54" black/white, panel GDEH0154D67. 200x200,
 *             ~2.5 s full refresh, ~0.5 s partial.
 *
 * The choice is build-time because it sets the framebuffer depth. Nothing
 * outside this component has to know which one is fitted - a caller that draws
 * in EINK_RED gets red on one panel and black on the other, and needs no
 * branch of its own.
 *
 * The panel rail belongs to this component: eink_init() brings it up,
 * eink_power_off() takes it down, and nothing outside touches it.
 *
 * Calls block. A refresh is seconds of waiting on BUSY, so this runs from a
 * task that is allowed to wait - it is not thread-safe and there is one panel.
 */
#pragma once

#include <stdbool.h>

#include "driver/spi_common.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_host_device_t spi_host;
    int pin_clk;
    int pin_mosi;
    int pin_cs;
    int pin_dc;
    int pin_rst;
    int pin_busy;
    int pin_power;         // load switch for the panel rail
    bool power_active_low; // a P-FET high-side switch conducts with its gate low
} eink_config_t;

/* Powers the panel rail, resets the controller, runs its init sequence and
 * allocates the framebuffer. The glass still shows whatever was on it, and the
 * canvas still holds whatever was last drawn - it survives a power cycle of the
 * panel, so waking up does not mean redrawing from scratch. */
esp_err_t eink_init(const eink_config_t *config);

/* Pushes the current canvas contents and waits for the refresh to finish.
 * Blocks for as long as the panel needs: seconds.
 *
 * Uses a partial refresh where the panel has one, promoting to a full refresh
 * every CONFIG_EINK_FULL_REFRESH_INTERVAL frames so ghosting cannot pile up.
 * `force_full` demands the full waveform - slower and it flashes, but it is the
 * only thing that clears ghosting, so it is what the first frame after a cold
 * start should use. On the four-colour panel every refresh is full and the flag
 * makes no difference. */
esp_err_t eink_refresh(bool force_full);

/* Bring-up patterns. Each answers one question, in the order you want them
 * answered, before any layout code is in the picture. */
typedef enum {
    EINK_PATTERN_WHITE,    // does the glass update at all
    EINK_PATTERN_BLACK,    // is black really black - polarity of the pixel bits
    EINK_PATTERN_QUADRANT, // orientation: black top-left quarter inside a 1 px frame
    EINK_PATTERN_CHECKER,  // packing: a wrong row stride shows up as diagonal stripes
    EINK_PATTERN_COLORS,   // four-colour panel only: white/black/yellow/red bands
} eink_pattern_t;

/* Draws one of the patterns above, overwriting the canvas, and pushes it with
 * the full waveform. Exists for bring-up: when a panel shows nothing, this
 * separates "the bus and the controller work" from "the layout is wrong", which
 * drawing the real screen cannot. */
esp_err_t eink_show_pattern(eink_pattern_t pattern);

/* True while the controller is still busy with the previous refresh. */
bool eink_is_busy(void);

/* Puts the controller into deep sleep and cuts the panel rail. Must run before
 * the MCU sleeps, otherwise the rail keeps draining the supply. The image stays
 * on the glass; e-paper needs no power to hold it.
 *
 * eink_init() has to run again afterwards: the controller answers nothing until
 * it has been reset. */
esp_err_t eink_power_off(void);

#ifdef __cplusplus
}
#endif
