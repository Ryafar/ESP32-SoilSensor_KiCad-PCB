/* E-ink display - what the application talks to.
 *
 * This header is the whole public surface of the component - everything else is
 * under priv_include/ and no caller may reach it. Inside, the display splits
 * into four files, each named for what it actually is:
 *
 *   eink.c         framebuffer, screen layout, refresh policy. This header.
 *   eink_canvas.c  drawing primitives and the pixel packing.
 *   epd_bus.c      SPI, D/C, RST, BUSY and the rail - the half both
 *                  controllers share.
 *   ssd1681.c      the black/white controller, register for register.
 *   jd79660.c      the four-colour one. Only one of the two is compiled.
 *
 * The controller files are named after their chips for the same reason the
 * charger driver is bq25186 rather than "charger": the name says which
 * datasheet the register writes came from. eink.c itself contains no controller
 * registers at all.
 *
 * Two panels are supported, picked in menuconfig under "E-ink display":
 *
 *   SSD1681   WeAct Studio 1.54" black/white, panel GDEH0154D67. Fitted today.
 *             200x200, ~2.5 s full refresh, ~0.5 s partial.
 *   JD79660   WeAct Studio 1.54" four colour, panel GDEM0154F51H. 200x200,
 *             black/white/red/yellow, ~21 s per refresh and no partial mode.
 *
 * The choice is build-time because it sets the framebuffer depth. Nothing else
 * in the firmware has to know which one is fitted.
 *
 * The panel rail on PAPER_POWER belongs to this component: eink_init() brings
 * it up, eink_power_off() takes it down, and nothing outside touches it.
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
    bool power_active_low; // AO3401A is a P-FET: gate to GND conducts. True on this board.
} eink_config_t;

/* What the application actually wants to show. Kept as data rather than as
 * drawing calls so the state machine never has to know about pixels. */
typedef struct {
    int soil_percent;    // -1 when there is no valid reading
    int battery_mv;
    bool charging;
    bool battery_low;
} eink_screen_t;

/* Powers the panel rail, resets the controller, runs its init sequence and
 * allocates the framebuffer. The glass still shows whatever was on it. */
esp_err_t eink_init(const eink_config_t *config);

/* Renders the screen and waits for the refresh to complete. Blocks for as long
 * as the panel needs - seconds, for a full refresh.
 *
 * Uses a partial refresh where the panel has one, promoting to a full refresh
 * every CONFIG_EINK_FULL_REFRESH_INTERVAL frames so ghosting cannot pile up
 * indefinitely. */
esp_err_t eink_render(const eink_screen_t *screen);

/* Forces the full waveform. Slower and it flashes, but it is the only thing
 * that clears ghosting, so it is what the first frame after a cold start and
 * the last frame before a long sleep should use. */
esp_err_t eink_render_full(const eink_screen_t *screen);

/* True while the controller is still busy with the previous refresh. */
bool eink_is_busy(void);

/* Puts the controller into deep sleep and cuts the panel rail. Must run before
 * the ESP32 sleeps, otherwise the rail keeps draining the cell. The image
 * stays on the glass; e-paper needs no power to hold it.
 *
 * eink_init() has to run again afterwards: the controller answers nothing until
 * it has been reset. */
esp_err_t eink_power_off(void);

#ifdef __cplusplus
}
#endif
