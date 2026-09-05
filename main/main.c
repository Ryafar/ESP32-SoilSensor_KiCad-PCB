/* Soil sensor firmware - application entry point.
 *
 * app_main creates the shared resources, initialises the drivers and starts the
 * tasks. Everything the tasks actually do lives in app/. Stack sizes and
 * priorities are decided here on purpose: scheduling policy belongs in one
 * visible place, not scattered across the modules that happen to need a task.
 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "rgb.h"
#include "rgb_test_task.h"

static const char *TAG = "soilsensor";

#define RGB_TEST_TASK_STACK 3072
#define RGB_TEST_TASK_PRIO 3

void app_main(void)
{
    ESP_LOGI(TAG, "soil sensor firmware, bring-up build");

    const rgb_config_t rgb_cfg = {
        .gpio_num = PIN_RGB_DIN,
        .brightness = 16,
    };
    ESP_ERROR_CHECK(rgb_init(&rgb_cfg));

    xTaskCreate(rgb_test_task, "rgb_test", RGB_TEST_TASK_STACK, NULL,
                RGB_TEST_TASK_PRIO, NULL);

    /* app_main returns; FreeRTOS deletes the main task and the created tasks
     * keep running. */
}
