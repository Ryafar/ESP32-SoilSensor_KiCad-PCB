/* Soil sensor firmware - application entry point.
 *
 * Currently only a status-LED heartbeat, kept as a bring-up smoke test for the
 * custom ESP32-C6 board. Sensor sampling and radio come on top of this.
 *
 * The status LED is an addressable RGB LED (WS2812), driven over RMT.
 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "sdkconfig.h"

static const char *TAG = "soilsensor";

#define STATUS_LED_GPIO CONFIG_SOILSENSOR_STATUS_LED_GPIO
#define STATUS_LED_BRIGHTNESS CONFIG_SOILSENSOR_STATUS_LED_BRIGHTNESS
#define HEARTBEAT_PERIOD_MS CONFIG_SOILSENSOR_HEARTBEAT_PERIOD_MS

static led_strip_handle_t s_status_led;

static void configure_status_led(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = STATUS_LED_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_status_led));
    ESP_ERROR_CHECK(led_strip_clear(s_status_led));
}

static void set_status_led(bool on)
{
    if (on) {
        ESP_ERROR_CHECK(led_strip_set_pixel(s_status_led, 0, STATUS_LED_BRIGHTNESS,
                                            STATUS_LED_BRIGHTNESS, STATUS_LED_BRIGHTNESS));
        ESP_ERROR_CHECK(led_strip_refresh(s_status_led));
    } else {
        ESP_ERROR_CHECK(led_strip_clear(s_status_led));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Soil sensor firmware starting, status LED on GPIO%d", STATUS_LED_GPIO);
    configure_status_led();

    bool led_on = false;
    while (1) {
        led_on = !led_on;
        set_status_led(led_on);
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
    }
}
