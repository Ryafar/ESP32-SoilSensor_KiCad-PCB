#include "app_net.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

#include "secrets.h"

static const char *TAG = "app_net";

/* Long enough for a DHCP lease on a busy network, short enough that a wrong
 * password does not stall the first measurement for a minute. */
#define WIFI_CONNECT_TIMEOUT_MS 15000

/* The driver retries on its own schedule after this; the point of a bounded
 * count here is to get "wrong password" into the log instead of an endless
 * quiet retry. */
#define WIFI_CONNECT_ATTEMPTS 5

#define GOT_IP_BIT BIT0

/* Home Assistant's default discovery prefix. */
#define DISCOVERY_PREFIX "homeassistant"

/* Topic and payload buffers. The state payload is six fixed fields, the
 * discovery payloads are the longest thing here - one device block plus one
 * entity. Sized with headroom and every write is a bounded snprintf. */
/* An empty credential means "anonymous", which esp-mqtt expresses as NULL, not
 * as an empty string - handing it "" makes it send a zero-length username, and
 * a broker that allows anonymous access can still reject that. sizeof works
 * here because these are string literals from secrets.h: sizeof("") is 1. */
#define OPTIONAL_CREDENTIAL(s) (sizeof(s) > 1 ? (s) : NULL)

#define TOPIC_LEN 128
#define STATE_PAYLOAD_LEN 256
#define DISCOVERY_PAYLOAD_LEN 768

static EventGroupHandle_t s_events;
static esp_mqtt_client_handle_t s_mqtt;
static bool s_mqtt_connected;
static bool s_discovery_sent;
static int s_wifi_attempts;

/* Derived from the station MAC, so two boards on the same broker do not fight
 * over one set of entities. */
static char s_device_id[24];
static char s_topic_state[TOPIC_LEN];
static char s_topic_status[TOPIC_LEN];

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        s_wifi_attempts++;
        /* The reason code is the whole diagnosis here: 15 is a wrong password,
         * 201 is an SSID that was never seen - 2.4 GHz vs 5 GHz, usually. */
        ESP_LOGW(TAG, "disconnected, reason %d (attempt %d)", event->reason,
                 s_wifi_attempts);
        if (s_wifi_attempts < WIFI_CONNECT_ATTEMPTS) {
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "giving up on the association for now; measurements "
                          "continue and the driver keeps retrying in the "
                          "background");
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "got " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_attempts = 0;
        xEventGroupSetBits(s_events, GOT_IP_BIT);
    }
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const esp_mqtt_event_handle_t event = data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected to %s", SECRET_MQTT_URI);
        s_mqtt_connected = true;
        /* Retained, so Home Assistant sees the device as available even if it
         * subscribes after this. The matching "offline" is the broker's job via
         * the last will below. */
        esp_mqtt_client_publish(s_mqtt, s_topic_status, "online", 0, 1, 1);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_mqtt_connected = false;
        break;
    case MQTT_EVENT_ERROR:
        /* Without this the failure mode is a silent reconnect loop. */
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            /* The socket errno is a plain errno, not an esp_err_t - printing it
             * through esp_err_to_name() turns 0 into a cheerful "ESP_OK" next
             * to a failure. 113/118 is "no route": wrong broker address in
             * secrets.h, or no association yet. */
            ESP_LOGE(TAG, "MQTT transport error: socket errno %d, esp-tls %s "
                          "(stack 0x%x) - broker %s",
                     event->error_handle->esp_transport_sock_errno,
                     esp_err_to_name(event->error_handle->esp_tls_last_esp_err),
                     event->error_handle->esp_tls_stack_err, SECRET_MQTT_URI);
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "broker refused the connection, code %d - usually "
                          "credentials or anonymous access being off",
                     event->error_handle->connect_return_code);
        }
        break;
    default:
        break;
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* The WiFi driver keeps its RF calibration here, so an unusable NVS has
         * to be erased rather than worked around. */
        ESP_LOGW(TAG, "NVS unusable, erasing it - RF calibration will be redone");
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase failed");
        err = nvs_flash_init();
    }
    return err;
}

static void build_identity(void)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_device_id, sizeof(s_device_id), "soilsensor_%02x%02x%02x", mac[3],
             mac[4], mac[5]);
    snprintf(s_topic_state, sizeof(s_topic_state), "soilsensor/%s/state", s_device_id);
    snprintf(s_topic_status, sizeof(s_topic_status), "soilsensor/%s/status", s_device_id);
    ESP_LOGI(TAG, "device id %s, state topic %s", s_device_id, s_topic_state);
}

static esp_err_t start_wifi(void)
{
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "nvs init failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_sta() != NULL, ESP_FAIL, TAG,
                        "could not create the STA netif");

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            wifi_event_handler, NULL, NULL),
                        TAG, "wifi handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            wifi_event_handler, NULL, NULL),
                        TAG, "ip handler failed");

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = SECRET_WIFI_SSID,
            .password = SECRET_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG,
                        "set config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
    return ESP_OK;
}

static esp_err_t start_mqtt(void)
{
    const esp_mqtt_client_config_t config = {
        .broker.address.uri = SECRET_MQTT_URI,
        .credentials.username = OPTIONAL_CREDENTIAL(SECRET_MQTT_USERNAME),
        .credentials.authentication.password = OPTIONAL_CREDENTIAL(SECRET_MQTT_PASSWORD),
        /* The broker publishes this if the device drops off without saying
         * goodbye - which, once deep sleep is in, is what every cycle looks
         * like. Retained so a late subscriber still learns the state. */
        .session.last_will = {
            .topic = s_topic_status,
            .msg = "offline",
            .qos = 1,
            .retain = 1,
        },
    };

    s_mqtt = esp_mqtt_client_init(&config);
    ESP_RETURN_ON_FALSE(s_mqtt != NULL, ESP_FAIL, TAG, "mqtt client init failed");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(s_mqtt, MQTT_EVENT_ANY,
                                                       mqtt_event_handler, NULL),
                        TAG, "mqtt handler failed");
    return esp_mqtt_client_start(s_mqtt);
}

/* One discovery message per entity. All four share the same state topic and
 * pick their value out of the JSON, which is why a single publish per cycle can
 * feed the whole device.
 *
 * `unique_id` is what lets Home Assistant remember the entity across reboots,
 * and the identical `device.identifiers` is what groups the four into one
 * device rather than four loose sensors. */
static esp_err_t publish_discovery_entity(const char *object_id, const char *component,
                                          const char *name, const char *extra)
{
    char topic[TOPIC_LEN];
    char payload[DISCOVERY_PAYLOAD_LEN];

    snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config", DISCOVERY_PREFIX, component,
             s_device_id, object_id);

    const int written = snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"%s\","
        "\"unique_id\":\"%s_%s\","
        "\"state_topic\":\"%s\","
        "\"availability_topic\":\"%s\","
        "\"json_attributes_topic\":\"%s\","
        "%s"
        "\"device\":{"
        "\"identifiers\":[\"%s\"],"
        "\"name\":\"Soil Sensor\","
        "\"model\":\"SoilSensor rev0.1.0\","
        "\"manufacturer\":\"Ryafar\""
        "}"
        "}",
        name, s_device_id, object_id, s_topic_state, s_topic_status, s_topic_state,
        extra, s_device_id);
    ESP_RETURN_ON_FALSE(written > 0 && written < (int)sizeof(payload), ESP_ERR_NO_MEM, TAG,
                        "discovery payload for %s does not fit", object_id);

    /* Retained: Home Assistant has to find the configuration whenever it
     * restarts, not only while the device happens to be publishing. */
    const int id = esp_mqtt_client_publish(s_mqtt, topic, payload, written, 1, 1);
    ESP_RETURN_ON_FALSE(id >= 0, ESP_FAIL, TAG, "discovery publish for %s failed",
                        object_id);
    return ESP_OK;
}

static esp_err_t publish_discovery(void)
{
    ESP_RETURN_ON_ERROR(publish_discovery_entity(
        "soil", "sensor", "Soil moisture",
        "\"unit_of_measurement\":\"%\","
        "\"state_class\":\"measurement\","
        "\"icon\":\"mdi:water-percent\","
        "\"value_template\":\"{{ value_json.soil_pct }}\","), TAG, "soil discovery");

    ESP_RETURN_ON_ERROR(publish_discovery_entity(
        "battery", "sensor", "Battery voltage",
        "\"device_class\":\"voltage\","
        "\"unit_of_measurement\":\"V\","
        "\"state_class\":\"measurement\","
        "\"value_template\":\"{{ (value_json.vbat_mv | float / 1000) | round(2) }}\","),
        TAG, "battery discovery");

    ESP_RETURN_ON_ERROR(publish_discovery_entity(
        "charging", "binary_sensor", "Charging",
        "\"device_class\":\"battery_charging\","
        "\"payload_on\":\"true\","
        "\"payload_off\":\"false\","
        "\"value_template\":\"{{ value_json.charging | lower }}\","),
        TAG, "charging discovery");

    /* Diagnostic: it says whether a missing publish was the radio or the
     * broker, which is the question that comes up first. */
    ESP_RETURN_ON_ERROR(publish_discovery_entity(
        "rssi", "sensor", "WiFi signal",
        "\"device_class\":\"signal_strength\","
        "\"unit_of_measurement\":\"dBm\","
        "\"state_class\":\"measurement\","
        "\"entity_category\":\"diagnostic\","
        "\"value_template\":\"{{ value_json.rssi }}\","), TAG, "rssi discovery");

    ESP_LOGI(TAG, "published Home Assistant discovery for %s", s_device_id);
    return ESP_OK;
}

esp_err_t app_net_start(void)
{
    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events != NULL, ESP_ERR_NO_MEM, TAG, "event group failed");

    ESP_RETURN_ON_ERROR(start_wifi(), TAG, "wifi startup failed");
    build_identity(); // needs the MAC, which needs the driver started

    const EventBits_t bits = xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE,
                                                 pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if ((bits & GOT_IP_BIT) == 0) {
        ESP_LOGW(TAG, "no IP after %d ms - starting MQTT anyway, it connects when "
                      "the association comes up", WIFI_CONNECT_TIMEOUT_MS);
    }

    ESP_RETURN_ON_ERROR(start_mqtt(), TAG, "mqtt startup failed");
    return (bits & GOT_IP_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool app_net_is_connected(void)
{
    return s_mqtt_connected;
}

esp_err_t app_net_publish(const app_sample_t *sample)
{
    ESP_RETURN_ON_FALSE(sample != NULL, ESP_ERR_INVALID_ARG, TAG, "no sample");
    if (!s_mqtt_connected) {
        ESP_LOGW(TAG, "not connected, sample %" PRIu32 " not published", sample->sequence);
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_discovery_sent) {
        /* Once per boot, and before the first state message, so Home Assistant
         * has somewhere to put the value it is about to receive. */
        ESP_RETURN_ON_ERROR(publish_discovery(), TAG, "discovery failed");
        s_discovery_sent = true;
    }

    wifi_ap_record_t ap = {0};
    const int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;

    char payload[STATE_PAYLOAD_LEN];
    const int written = snprintf(payload, sizeof(payload),
        "{\"seq\":%" PRIu32 ",\"soil_mv\":%d,\"soil_pct\":%d,\"vbat_mv\":%d,"
        "\"charging\":%s,\"usb\":%s,\"rssi\":%d}",
        sample->sequence, sample->soil_mv, sample->soil_percent, sample->battery_mv,
        sample->charging ? "true" : "false", sample->usb_present ? "true" : "false",
        rssi);
    ESP_RETURN_ON_FALSE(written > 0 && written < (int)sizeof(payload), ESP_ERR_NO_MEM, TAG,
                        "state payload does not fit");

    /* QoS 1, not retained: a soil reading that is an hour old is not worth
     * handing to the next subscriber as if it were current. */
    const int id = esp_mqtt_client_publish(s_mqtt, s_topic_state, payload, written, 1, 0);
    ESP_RETURN_ON_FALSE(id >= 0, ESP_FAIL, TAG, "state publish failed");
    ESP_LOGI(TAG, "published %s", payload);
    return ESP_OK;
}
