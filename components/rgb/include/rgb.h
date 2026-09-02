/* Status LED driver for the Wuerth WL-ICLED 1312020030000.
 *
 * The part is a single-wire 24-bit ICLED speaking the WS2812 protocol, so the
 * bit timing is handled by espressif/led_strip on the RMT peripheral. This
 * component adds the board-level status semantics on top.
 *
 * The driver is board-agnostic: the GPIO comes in through rgb_init(), the
 * board's pin map lives in main/config.h.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status colors. The mapping to RGB values lives in rgb.c so that callers talk
 * about device state, not about color. */
typedef enum {
    RGB_STATUS_OFF = 0,
    RGB_STATUS_BOOT,        // white, board came up
    RGB_STATUS_MEASURING,   // blue, sampling soil or battery
    RGB_STATUS_CHARGING,    // yellow, charger reports charge in progress
    RGB_STATUS_BATTERY_LOW, // red, below the ship-mode threshold
    RGB_STATUS_FAULT,       // magenta, charger fault or failed measurement
} rgb_status_t;

typedef struct {
    int gpio_num;       // data-in pin of the ICLED
    uint8_t brightness; // 0-255, scales every color; the LED is bright, 16 is plenty
} rgb_config_t;

/* Claims one RMT TX channel. The LED is dark when this returns. */
esp_err_t rgb_init(const rgb_config_t *config);

/* Releases the RMT channel. Call before deep sleep so the peripheral is off. */
esp_err_t rgb_deinit(void);

/* Raw color, before brightness scaling. */
esp_err_t rgb_set(uint8_t red, uint8_t green, uint8_t blue);

esp_err_t rgb_set_status(rgb_status_t status);

esp_err_t rgb_off(void);

/* Blocking blink, meant for bring-up and brief status feedback. */
esp_err_t rgb_blink(rgb_status_t status, uint32_t on_ms, uint32_t off_ms, uint32_t times);

#ifdef __cplusplus
}
#endif
