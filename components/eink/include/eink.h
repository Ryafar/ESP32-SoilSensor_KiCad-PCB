/* E-ink panel driver - PLACEHOLDER.
 *
 * The panel's controller IC is not identified yet (open item in
 * docs/hardware/README.md §9), and that decision drives everything below: the
 * init sequence, the command set, the LUT handling and whether an existing
 * driver can be used at all. Until then this component defines the shape of
 * the interface so the rest of the firmware can be written against it, and
 * every entry point returns ESP_ERR_NOT_SUPPORTED.
 *
 * Once the controller is known this component gets renamed after it - ssd1680,
 * uc8151, whatever it turns out to be - the same way the charger driver is
 * named bq25186 rather than "charger". What stays generic is only the part of
 * the API the application talks to.
 *
 * What is already fixed by the schematic: 4-wire SPI, write-only, with BUSY,
 * RST and D/C as separate lines, and the panel rail behind a load switch on
 * PAPER_POWER. The rail is owned by this driver: it is switched on in
 * eink_init() and off in eink_power_off(), and nothing outside touches it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/spi_master.h"
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
    int pin_power;      // load switch for the panel rail
    bool power_active_low; // AO3401A is a P-FET, so this is likely true - unconfirmed
    int width;
    int height;
} eink_config_t;

/* What the application actually wants to show. Kept as data rather than as
 * drawing calls so the state machine never has to know about pixels. */
typedef struct {
    int soil_percent;    // -1 when there is no valid reading
    int battery_mv;
    bool charging;
    bool battery_low;
} eink_screen_t;

/* Powers the panel rail, resets the controller and runs its init sequence. */
esp_err_t eink_init(const eink_config_t *config);

/* Renders the screen and waits for the refresh to complete. Blocks for as long
 * as the panel needs - seconds, for a full refresh. */
esp_err_t eink_render(const eink_screen_t *screen);

/* Full refresh clears ghosting and is slow; partial is fast and accumulates
 * artefacts. The application decides how often a full one is worth it. */
esp_err_t eink_render_full(const eink_screen_t *screen);

/* True while the controller is still busy with the previous refresh. */
bool eink_is_busy(void);

/* Puts the controller into deep sleep and cuts the panel rail. Must run before
 * the ESP32 sleeps, otherwise the rail keeps draining the cell. */
esp_err_t eink_power_off(void);

#ifdef __cplusplus
}
#endif
