#include "charger_test_task.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bq25186.h"

static const char *TAG = "charger_test";

#define STATUS_POLL_MS 10000
#define PROBE_RETRY_MS 2000

void charger_test_task(void *arg)
{
    (void)arg;

    /* Retry rather than give up: on a bench the charger may only answer once
     * USB is plugged in, and a task that already exited cannot tell you that. */
    while (bq25186_probe() != ESP_OK) {
        ESP_LOGE(TAG, "no ACK at 0x%02X - check SDA/SCL wiring, pull-ups, and "
                      "whether the chip has power", BQ25186_I2C_ADDR_DEFAULT);
        vTaskDelay(pdMS_TO_TICKS(PROBE_RETRY_MS));
    }

    ESP_LOGI(TAG, "register dump - compare against the datasheet reset values");
    const esp_err_t err = bq25186_dump_registers();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dump failed: %s", esp_err_to_name(err));
    }

    uint16_t ichg_ma = 0;
    if (bq25186_get_charge_current(&ichg_ma) == ESP_OK) {
        ESP_LOGI(TAG, "charge current currently set to %u mA%s", ichg_ma,
                 ichg_ma == 10 ? " (reset default, charger unconfigured)" : "");
    }

    while (1) {
        bq25186_status_t status;
        if (bq25186_read_status(&status) == ESP_OK) {
            /* Raw bytes on purpose - the bit meanings are not confirmed yet,
             * and a wrong flag name would be worse than a hex value. */
            ESP_LOGI(TAG, "STAT0=0x%02X STAT1=0x%02X FLAG0=0x%02X",
                     status.stat0, status.stat1, status.flag0);
        } else {
            ESP_LOGW(TAG, "status read failed, charger stopped responding");
        }
        vTaskDelay(pdMS_TO_TICKS(STATUS_POLL_MS));
    }
}
