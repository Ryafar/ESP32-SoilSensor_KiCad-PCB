#include "wifi_test_task.h"

#include <stdlib.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_test";

#define SCAN_PERIOD_MS 10000

/* Per-channel dwell. Thirteen channels at the maximum is about four seconds,
 * which is what it costs not to miss an AP that beacons slowly. */
#define SCAN_DWELL_MIN_MS 100
#define SCAN_DWELL_MAX_MS 300

/* Enough to characterise the RF path; a dense office would otherwise fill the
 * log with APs that say nothing new. */
#define SCAN_MAX_RECORDS 32

/* Roughly where a link stops being comfortable. Only used to give the summary
 * line a second number that does not move around as much as the median. */
#define RSSI_USABLE_DBM (-70)

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* The WiFi driver keeps its RF calibration data here, so an unusable
         * NVS has to be erased rather than worked around. */
        ESP_LOGW(TAG, "NVS unusable, erasing it - RF calibration will be redone");
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase failed");
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t init_wifi(void)
{
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "nvs init failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");

    /* Not needed to scan - there is no IP traffic here - but creating it keeps
     * the driver in the configuration everything else expects. */
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_sta() != NULL, ESP_FAIL, TAG,
                        "could not create the STA netif");

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
    return ESP_OK;
}

/* Logged once. All three are things that make a weak scan mean something other
 * than a bad antenna, so they belong in the log next to the readings. */
static void log_radio_info(void)
{
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        ESP_LOGI(TAG, "STA MAC %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
                 mac[2], mac[3], mac[4], mac[5]);
    }

    int8_t power = 0;
    if (esp_wifi_get_max_tx_power(&power) == ESP_OK) {
        /* The driver reports quarter-dBm units. */
        ESP_LOGI(TAG, "max TX power %d.%02d dBm", power / 4, (power % 4) * 25);
    }

    wifi_country_t country;
    if (esp_wifi_get_country(&country) == ESP_OK) {
        ESP_LOGI(TAG, "country %.2s, channels %d-%d - an AP outside that range "
                      "will not show up below", country.cc, country.schan,
                 country.schan + country.nchan - 1);
    }
}

static int by_rssi_desc(const void *a, const void *b)
{
    const wifi_ap_record_t *lhs = a;
    const wifi_ap_record_t *rhs = b;
    return rhs->rssi - lhs->rssi;
}

static void log_scan_results(unsigned round, uint16_t found,
                             const wifi_ap_record_t *records, uint16_t count)
{
    for (uint16_t i = 0; i < count; i++) {
        const wifi_ap_record_t *ap = &records[i];
        ESP_LOGI(TAG, "  %4d dBm  ch %2d  %02x:%02x:%02x:%02x:%02x:%02x  %s",
                 ap->rssi, ap->primary, ap->bssid[0], ap->bssid[1], ap->bssid[2],
                 ap->bssid[3], ap->bssid[4], ap->bssid[5],
                 ap->ssid[0] != '\0' ? (const char *)ap->ssid : "<hidden>");
    }

    int usable = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (records[i].rssi >= RSSI_USABLE_DBM) {
            usable++;
        }
    }

    /* Sorted descending, so the strongest is first and the median is the middle
     * one. The median is the honest summary of the environment; the strongest
     * is the number to compare against a phone. */
    ESP_LOGI(TAG, "scan #%u: %u AP(s)%s, strongest %d dBm, median %d dBm, "
                  "%d at or above %d dBm",
             round, found, found > count ? ", list truncated" : "",
             records[0].rssi, records[count / 2].rssi, usable, RSSI_USABLE_DBM);
}

static void scan_once(unsigned round)
{
    const wifi_scan_config_t scan_config = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = SCAN_DWELL_MIN_MS, .max = SCAN_DWELL_MAX_MS},
    };

    /* Blocking: there is nothing else for this task to do while the radio walks
     * the channels, and a callback would only move the logging elsewhere. */
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan #%u failed to start: %s", round, esp_err_to_name(err));
        return;
    }

    uint16_t found = 0;
    if (esp_wifi_scan_get_ap_num(&found) != ESP_OK) {
        ESP_LOGE(TAG, "scan #%u: could not read the result count", round);
        esp_wifi_clear_ap_list();
        return;
    }
    if (found == 0) {
        /* Not a verdict on its own - an empty scan looks the same whether the
         * RF path is dead or there is genuinely nothing in range. */
        ESP_LOGW(TAG, "scan #%u: nothing found. Check a phone in the same spot "
                      "before suspecting the antenna", round);
        esp_wifi_clear_ap_list();
        return;
    }

    const uint16_t wanted = found < SCAN_MAX_RECORDS ? found : SCAN_MAX_RECORDS;
    wifi_ap_record_t *records = calloc(wanted, sizeof(*records));
    if (records == NULL) {
        ESP_LOGE(TAG, "out of memory for %u records", wanted);
        esp_wifi_clear_ap_list();
        return;
    }

    /* Takes the list away from the driver, so it must not be cleared as well. */
    uint16_t count = wanted;
    err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan #%u: could not read the records: %s", round,
                 esp_err_to_name(err));
    } else if (count > 0) {
        qsort(records, count, sizeof(*records), by_rssi_desc);
        log_scan_results(round, found, records, count);
    }

    free(records);
}

void wifi_test_task(void *arg)
{
    (void)arg;

    if (init_wifi() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi would not start - nothing below this line says "
                      "anything about the antenna");
        vTaskDelete(NULL);
        return;
    }
    log_radio_info();

    for (unsigned round = 1;; round++) {
        scan_once(round);
        vTaskDelay(pdMS_TO_TICKS(SCAN_PERIOD_MS));
    }
}
