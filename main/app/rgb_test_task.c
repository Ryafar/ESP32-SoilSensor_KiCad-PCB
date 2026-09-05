#include "rgb_test_task.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "rgb.h"

static const char *TAG = "rgb_test";

#define COLOR_HOLD_MS 1000

static const struct {
    const char *name;
    uint8_t r, g, b;
} k_cycle[] = {
    {"red", 255, 0, 0},
    {"green", 0, 255, 0},
    {"blue", 0, 0, 255},
    {"off", 0, 0, 0},
};

void rgb_test_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "starting colour cycle, compare against what the LED shows");

    while (1) {
        for (size_t i = 0; i < sizeof(k_cycle) / sizeof(k_cycle[0]); i++) {
            ESP_LOGI(TAG, "LED should be %s", k_cycle[i].name);

            const esp_err_t err = rgb_set(k_cycle[i].r, k_cycle[i].g, k_cycle[i].b);
            if (err != ESP_OK) {
                /* Bring-up code, but still no abort: a task that dies on a
                 * failed LED write takes the whole test session with it. */
                ESP_LOGE(TAG, "rgb_set failed: %s", esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(COLOR_HOLD_MS));
        }
    }
}
