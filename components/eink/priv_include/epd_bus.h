/* Transport for the e-ink panel: SPI, the three control lines and the rail.
 *
 * Private to the eink component. Both panel controllers this project supports
 * - SSD1681 on the black/white
 * module and JD79660 on the 4-colour one - talk the same 4-wire, write-only
 * protocol: pull CS low, D/C selects command or data, clock out bytes, release
 * CS. Only the register contents differ. That shared half lives here so the two
 * controller drivers contain nothing but their own command sequences.
 *
 * Two decisions worth stating, because both look like accidents otherwise:
 *
 * CS is a plain GPIO, not the SPI peripheral's hardware CS. A frame is 5-10 kB
 * and gets chunked on the way out; with hardware CS the line would deassert
 * between chunks in the middle of a RAM write. The SSD1681 tolerates that (the
 * vendor's own code toggles CS around every single byte) but the JD79660's
 * reference driver holds CS for the whole burst, and there is no reason to bet
 * the frame on which of the two is really required.
 *
 * The SPI bus is installed in epd_bus_power_on() and freed in
 * epd_bus_power_off(), rather than once at init. The rail is behind a load
 * switch, and while the rail is down the panel must not be fed through its
 * protection diodes - which is exactly what an idle SPI peripheral holding its
 * pins would do. Freeing the bus lets power_off park every line low before the
 * switch opens, so the rail actually reaches 0 V instead of floating at a diode
 * drop below the ESP32's output high.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
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
    int pin_power;         // load switch for the panel rail
    bool power_active_low; // AO3401A is a P-FET: the gate is pulled low to conduct
    int clock_hz;
    int busy_active_level; // level the BUSY pin sits at while the panel is busy
} epd_bus_config_t;

typedef struct epd_bus *epd_bus_handle_t;

/* Validates the configuration and takes a copy of it. Touches no hardware:
 * nothing is driven and no rail moves until epd_bus_power_on(). */
esp_err_t epd_bus_new(const epd_bus_config_t *config, epd_bus_handle_t *out_bus);

/* Powers the rail down if it is still up, then releases the handle. */
esp_err_t epd_bus_delete(epd_bus_handle_t bus);

/* Closes the load switch, waits for the rail to come up and installs SPI.
 * Idempotent. */
esp_err_t epd_bus_power_on(epd_bus_handle_t bus);

/* Frees SPI, parks CLK/MOSI/CS/DC/RST low so nothing back-feeds the panel
 * through its ESD diodes, and opens the load switch. The rail is down when this
 * returns. Idempotent. */
esp_err_t epd_bus_power_off(epd_bus_handle_t bus);

/* Hardware reset pulse, then waits for the controller to release BUSY. The
 * timings are the controller's, so they come from the caller. */
esp_err_t epd_bus_reset(epd_bus_handle_t bus, int high_ms, int low_ms, int settle_ms,
                        int busy_timeout_ms);

esp_err_t epd_bus_command(epd_bus_handle_t bus, uint8_t command);

/* Command followed by its parameter bytes, in one call because that is how
 * every line of a controller init sequence reads. */
esp_err_t epd_bus_command_with_data(epd_bus_handle_t bus, uint8_t command,
                                    const uint8_t *data, size_t length);

/* Data bytes with CS held low for the whole burst. The buffer may live
 * anywhere - it is copied through an internal DMA-capable bounce buffer. */
esp_err_t epd_bus_data(epd_bus_handle_t bus, const void *data, size_t length);

/* Same, with one repeated byte. Used to paint the panel a solid colour without
 * anyone having to allocate a frame first. */
esp_err_t epd_bus_data_fill(epd_bus_handle_t bus, uint8_t value, size_t length);

/* True while the controller holds BUSY asserted. */
bool epd_bus_is_busy(epd_bus_handle_t bus);

/* Polls BUSY until the controller releases it. Returns ESP_ERR_TIMEOUT if it
 * does not, which is the one failure mode this bus can actually detect - a
 * write-only panel acknowledges nothing else. */
esp_err_t epd_bus_wait_idle(epd_bus_handle_t bus, int timeout_ms);

#ifdef __cplusplus
}
#endif
