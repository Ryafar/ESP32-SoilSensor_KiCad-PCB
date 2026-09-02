#include "rgb.h"

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "rgb";

/* 10 MHz gives the RMT encoder 100 ns resolution, which is what led_strip's
 * WS2812 timings are built around. */
#define RGB_RMT_RESOLUTION_HZ (10 * 1000 * 1000)

static led_strip_handle_t s_strip;
static uint8_t s_brightness;

static const struct {
    uint8_t r, g, b;
} s_status_colors[] = {
    [RGB_STATUS_OFF] = {0, 0, 0},
    [RGB_STATUS_BOOT] = {255, 255, 255},
    [RGB_STATUS_MEASURING] = {0, 0, 255},
    [RGB_STATUS_CHARGING] = {255, 190, 0},
    [RGB_STATUS_BATTERY_LOW] = {255, 0, 0},
    [RGB_STATUS_FAULT] = {255, 0, 255},
};

esp_err_t rgb_init(const rgb_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(s_strip == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialized");

    led_strip_config_t strip_config = {
        .strip_gpio_num = config->gpio_num,
        .max_leds = 1,
        /* The WL-ICLED byte order is not confirmed yet - if red and green come
         * out swapped, this is the knob to turn. See docs/hardware/README.md. */
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = RGB_RMT_RESOLUTION_HZ,
        .flags.with_dma = false,
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip),
                        TAG, "failed to create RMT device on GPIO%d", config->gpio_num);

    s_brightness = config->brightness;
    ESP_LOGI(TAG, "status LED on GPIO%d, brightness %u", config->gpio_num, s_brightness);
    return led_strip_clear(s_strip);
}

esp_err_t rgb_deinit(void)
{
    ESP_RETURN_ON_FALSE(s_strip != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    ESP_RETURN_ON_ERROR(led_strip_del(s_strip), TAG, "failed to release RMT device");
    s_strip = NULL;
    return ESP_OK;
}

static uint8_t scale(uint8_t value)
{
    return (uint8_t)(((uint16_t)value * s_brightness) / 255);
}

esp_err_t rgb_set(uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_RETURN_ON_FALSE(s_strip != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_strip, 0, scale(red), scale(green), scale(blue)),
                        TAG, "failed to set pixel");
    return led_strip_refresh(s_strip);
}

esp_err_t rgb_set_status(rgb_status_t status)
{
    ESP_RETURN_ON_FALSE(status < sizeof(s_status_colors) / sizeof(s_status_colors[0]),
                        ESP_ERR_INVALID_ARG, TAG, "unknown status %d", status);

    if (status == RGB_STATUS_OFF) {
        return rgb_off();
    }
    return rgb_set(s_status_colors[status].r, s_status_colors[status].g, s_status_colors[status].b);
}

esp_err_t rgb_off(void)
{
    ESP_RETURN_ON_FALSE(s_strip != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    return led_strip_clear(s_strip);
}

esp_err_t rgb_blink(rgb_status_t status, uint32_t on_ms, uint32_t off_ms, uint32_t times)
{
    for (uint32_t i = 0; i < times; i++) {
        ESP_RETURN_ON_ERROR(rgb_set_status(status), TAG, "blink on failed");
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        ESP_RETURN_ON_ERROR(rgb_off(), TAG, "blink off failed");
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
    return ESP_OK;
}
