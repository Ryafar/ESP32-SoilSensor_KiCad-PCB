/* Soil sensor firmware - application entry point.
 *
 * Bring-up stage: exercises the RGB status LED. The color cycle below doubles
 * as the check for the open byte-order question in docs/hardware/README.md -
 * if the LED shows a different color than the log announces, the WL-ICLED
 * wants RGB instead of the GRB currently configured in components/rgb.
 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "rgb.h"

static const char *TAG = "soilsensor";

#define COLOR_HOLD_MS 1000

void app_main(void)
{
    ESP_LOGI(TAG, "Hello world!");

    const rgb_config_t rgb_cfg = {
        .gpio_num = PIN_RGB_DIN,
        .brightness = 16,
    };
    ESP_ERROR_CHECK(rgb_init(&rgb_cfg));

    const struct {
        const char *name;
        uint8_t r, g, b;
    } cycle[] = {
        {"red", 255, 0, 0},
        {"green", 0, 255, 0},
        {"blue", 0, 0, 255},
        {"off", 0, 0, 0},
    };

    while (1) {
        for (size_t i = 0; i < sizeof(cycle) / sizeof(cycle[0]); i++) {
            ESP_LOGI(TAG, "LED should be %s", cycle[i].name);
            ESP_ERROR_CHECK(rgb_set(cycle[i].r, cycle[i].g, cycle[i].b));
            vTaskDelay(pdMS_TO_TICKS(COLOR_HOLD_MS));
        }
    }
}
