#include "app_net.h"

#include <inttypes.h>
#include <stdio.h>
#include <time.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

#include "app_buffer.h"
#include "config.h"
#include "secrets.h"

static const char *TAG = "app_net";

/* Long enough for a DHCP lease on a busy network, short enough that a wrong
 * password does not stall the first measurement for a minute. */
#define WIFI_CONNECT_TIMEOUT_MS 15000

/* The driver retries on its own schedule after this; the point of a bounded
 * count here is to get "wrong password" into the log instead of an endless
 * quiet retry. */
#define WIFI_CONNECT_ATTEMPTS 5

/* How long to wait for the broker to acknowledge the state publish. Generous:
 * the alternative to waiting is losing the sample in deep sleep, and a slow
 * broker is not a reason to throw a reading away. */
#define PUBLISH_ACK_TIMEOUT_MS 5000

/* How long to wait for the MQTT CONNECT to complete before giving up on this
 * cycle. app_net_start() returns as soon as there is an IP address, because the
 * measurement should not wait on the broker - but the publish has to, or a
 * broker that takes half a second to answer means the samples are buffered
 * every single cycle while the network is perfectly fine. */
#define MQTT_CONNECT_TIMEOUT_MS 10000

#define GOT_IP_BIT BIT0
#define PUBLISHED_BIT BIT1
#define CONNECTED_BIT BIT2

/* Home Assistant's default discovery prefix. */
#define DISCOVERY_PREFIX "homeassistant"

/* Home Assistant marks an entity unavailable if no state arrives within this
 * many seconds. Three publish intervals: one missed cycle is normal enough (a
 * failed association, a broker restart) that it should not paint the device as
 * dead, three in a row is not.
 *
 * This replaces an availability topic with a last will. A device that sleeps
 * between cycles disconnects ungracefully by design, so a last will would
 * publish "offline" on almost every cycle and the availability state would say
 * nothing about the device. A timeout does. */
#define EXPIRE_AFTER_S (3 * MEASUREMENT_INTERVAL_S * PUBLISH_EVERY_N_MEASUREMENTS)

/* An empty credential means "anonymous", which esp-mqtt expresses as NULL, not
 * as an empty string - handing it "" makes it send a zero-length username, and
 * a broker that allows anonymous access can still reject that. sizeof works
 * here because these are string literals from secrets.h: sizeof("") is 1. */
#define OPTIONAL_CREDENTIAL(s) (sizeof(s) > 1 ? (s) : NULL)

/* Topic and payload buffers. The state payload is seven fixed fields, the
 * discovery payloads are the longest thing here - one device block plus one
 * entity. Sized with headroom and every write is a bounded snprintf. */
#define TOPIC_LEN 128
#define DISCOVERY_PAYLOAD_LEN 768

/* The state payload carries the newest reading plus the whole buffered bundle.
 * A bundle entry is about 70 characters, APP_BUFFER_SLOTS of them is under
 * 1.8 kB, and the rest is the fixed head - 3 kB leaves room. Every write is a
 * bounded snprintf and the bundle stops early rather than overflowing.
 *
 * It lives in .bss, not on the stack, and that is not a style choice: with deep
 * sleep the whole sequence runs on the main task, and 3 kB of payload plus a
 * discovery frame on top overflowed its 4 kB stack on the first publish. The
 * stack protector caught it. A buffer this size belongs in static memory, where
 * its cost is visible in the link map instead of depending on which task
 * happens to call. Single-threaded by construction - one panel, one radio, one
 * sequence - so sharing one buffer is safe. */
#define STATE_PAYLOAD_LEN 3072

/* esp-mqtt's default outbox buffer is 1 kB, which a full bundle exceeds - and
 * the failure mode is a publish that silently never goes out. Sized from the
 * payload rather than guessed. */
#define MQTT_BUFFER_LEN (STATE_PAYLOAD_LEN + 512)

static EventGroupHandle_t s_events;
static esp_mqtt_client_handle_t s_mqtt;
static bool s_mqtt_connected;
/* Survives deep sleep: the discovery configuration is retained on the broker,
 * so re-sending it on every wake would be five needless publishes a cycle.
 * Cleared only by a cold boot, which is exactly when Home Assistant might have
 * been restarted or the payload changed. */
static RTC_DATA_ATTR bool s_discovery_sent;
static int s_wifi_attempts;

/* Derived from the station MAC, so two boards on the same broker do not fight
 * over one set of entities. */
static char s_device_id[24];
static char s_topic_state[TOPIC_LEN];

/* See the note on STATE_PAYLOAD_LEN: these are static so they cannot overflow
 * the caller's stack. */
static char s_payload[STATE_PAYLOAD_LEN];
static char s_discovery_payload[DISCOVERY_PAYLOAD_LEN];
static char s_discovery_topic[TOPIC_LEN];
static int s_pending_msg_id = -1;

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
        xEventGroupSetBits(s_events, CONNECTED_BIT);
        break;
    case MQTT_EVENT_PUBLISHED:
        /* The broker has the message. Only the state publish is waited on, and
         * MQTT delivers in order on one connection, so an acknowledged state
         * message means the discovery publishes ahead of it arrived too. */
        if (event->msg_id == s_pending_msg_id) {
            xEventGroupSetBits(s_events, PUBLISHED_BIT);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_mqtt_connected = false;
        xEventGroupClearBits(s_events, CONNECTED_BIT);
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
        /* No last will. A device that deep-sleeps between cycles always
         * disconnects ungracefully, so a will would fire on nearly every cycle
         * and mean nothing; EXPIRE_AFTER_S in the discovery payload is what
         * tells Home Assistant the device has gone quiet. */
        .buffer.size = MQTT_BUFFER_LEN,
        .buffer.out_size = MQTT_BUFFER_LEN,
    };

    s_mqtt = esp_mqtt_client_init(&config);
    ESP_RETURN_ON_FALSE(s_mqtt != NULL, ESP_FAIL, TAG, "mqtt client init failed");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(s_mqtt, MQTT_EVENT_ANY,
                                                       mqtt_event_handler, NULL),
                        TAG, "mqtt handler failed");
    return esp_mqtt_client_start(s_mqtt);
}

/* One discovery message per entity. They all share the same state topic and
 * pick their value out of the JSON, which is why a single publish per cycle can
 * feed the whole device.
 *
 * `unique_id` is what lets Home Assistant remember the entity across reboots,
 * and the identical `device.identifiers` is what groups them into one device
 * rather than a handful of loose sensors. */
static esp_err_t publish_discovery_entity(const char *object_id, const char *component,
                                          const char *name, const char *extra)
{
    char *const topic = s_discovery_topic;
    char *const payload = s_discovery_payload;

    snprintf(topic, TOPIC_LEN, "%s/%s/%s/%s/config", DISCOVERY_PREFIX, component,
             s_device_id, object_id);

    const int written = snprintf(payload, DISCOVERY_PAYLOAD_LEN,
        "{"
        "\"name\":\"%s\","
        "\"unique_id\":\"%s_%s\","
        "\"state_topic\":\"%s\","
        "\"json_attributes_topic\":\"%s\","
        "\"expire_after\":%d,"
        "%s"
        "\"device\":{"
        "\"identifiers\":[\"%s\"],"
        "\"name\":\"Soil Sensor\","
        "\"model\":\"SoilSensor rev0.1.0\","
        "\"manufacturer\":\"Ryafar\""
        "}"
        "}",
        name, s_device_id, object_id, s_topic_state, s_topic_state, EXPIRE_AFTER_S,
        extra, s_device_id);
    ESP_RETURN_ON_FALSE(written > 0 && written < DISCOVERY_PAYLOAD_LEN, ESP_ERR_NO_MEM, TAG,
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
        "\"suggested_display_precision\":0,"
        "\"icon\":\"mdi:water-percent\","
        "\"value_template\":\"{{ value_json.soil_pct }}\","), TAG, "soil discovery");

    /* The probe's own output, next to the percentage that is derived from it.
     * Worth its own entity as long as the wet calibration point is a
     * placeholder: this is the number that stays true when the mapping changes,
     * and graphing it is how the real wet point gets found. */
    ESP_RETURN_ON_ERROR(publish_discovery_entity(
        "soil_voltage", "sensor", "Soil voltage",
        "\"device_class\":\"voltage\","
        "\"unit_of_measurement\":\"V\","
        "\"state_class\":\"measurement\","
        "\"suggested_display_precision\":3,"
        "\"value_template\":\"{{ value_json.soil_mv / 1000 }}\","),
        TAG, "soil voltage discovery");

    /* Three decimals, not two: the ADC resolves single millivolts, and the
     * whole point of this entity is watching a cell drift by a few mV over
     * hours. Rounding in the template would throw that away before Home
     * Assistant ever sees it - the state carries full resolution and
     * suggested_display_precision decides what the dashboard shows, which is a
     * display setting the user can still change per entity. */
    ESP_RETURN_ON_ERROR(publish_discovery_entity(
        "battery", "sensor", "Battery voltage",
        "\"device_class\":\"voltage\","
        "\"unit_of_measurement\":\"V\","
        "\"state_class\":\"measurement\","
        "\"suggested_display_precision\":3,"
        "\"value_template\":\"{{ value_json.vbat_mv / 1000 }}\","),
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

/* One bundle entry. `age_s` rather than a timestamp: there is no SNTP yet, so
 * an absolute time would be seconds since this board last cold-booted, which no
 * consumer can interpret. An age can be subtracted from the time of receipt and
 * is correct whether or not a clock was ever synchronised (product design §5).
 *
 * Returns the number of characters written, or a negative value if it did not
 * fit - the caller stops the bundle there rather than truncating JSON. */
static int append_sample(char *out, size_t room, const app_sample_t *sample, uint32_t now_s)
{
    const uint32_t age_s = now_s >= sample->taken_at_s ? now_s - sample->taken_at_s : 0;

    const int written = snprintf(out, room,
        "{\"seq\":%" PRIu32 ",\"age_s\":%" PRIu32 ",\"soil_mv\":%d,"
        "\"soil_pct\":%d,\"vbat_mv\":%d,\"charging\":%s,\"usb\":%s}",
        sample->sequence, age_s, sample->soil_mv, sample->soil_percent,
        sample->battery_mv, sample->charging ? "true" : "false",
        sample->usb_present ? "true" : "false");

    return (written > 0 && (size_t)written < room) ? written : -1;
}

esp_err_t app_net_publish(const app_sample_t *samples, size_t count)
{
    ESP_RETURN_ON_FALSE(samples != NULL && count > 0, ESP_ERR_INVALID_ARG, TAG,
                        "nothing to publish");

    if (!s_mqtt_connected) {
        /* The client connects in the background while the measurement runs, so
         * arriving here before it finished is normal, not a failure. Waiting is
         * the whole point: giving up now would buffer a sample the broker was
         * a few hundred milliseconds away from accepting. */
        ESP_LOGI(TAG, "waiting up to %d ms for the broker", MQTT_CONNECT_TIMEOUT_MS);
        const EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT, pdFALSE,
                                                     pdTRUE,
                                                     pdMS_TO_TICKS(MQTT_CONNECT_TIMEOUT_MS));
        if ((bits & CONNECTED_BIT) == 0) {
            ESP_LOGW(TAG, "no broker connection, %u samples stay buffered",
                     (unsigned)count);
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (!s_discovery_sent) {
        /* Once per cold boot, and before the first state message, so Home
         * Assistant has somewhere to put the value it is about to receive. */
        ESP_RETURN_ON_ERROR(publish_discovery(), TAG, "discovery failed");
        s_discovery_sent = true;
    }

    wifi_ap_record_t ap = {0};
    const int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    const uint32_t now_s = (uint32_t)time(NULL);

    /* The newest sample is the entity state - that is what Home Assistant
     * graphs, and it is the only part of this payload with a value_template
     * pointing at it. The bundle is an attribute. */
    const app_sample_t *newest = &samples[count - 1];

    char *const payload = s_payload;
    int at = snprintf(payload, STATE_PAYLOAD_LEN,
        "{\"seq\":%" PRIu32 ",\"soil_mv\":%d,\"soil_pct\":%d,\"vbat_mv\":%d,"
        "\"charging\":%s,\"usb\":%s,\"rssi\":%d,\"buffered\":%u,"
        "\"dropped\":%" PRIu32 ",\"samples\":[",
        newest->sequence, newest->soil_mv, newest->soil_percent, newest->battery_mv,
        newest->charging ? "true" : "false", newest->usb_present ? "true" : "false",
        rssi, (unsigned)count, app_buffer_dropped());
    ESP_RETURN_ON_FALSE(at > 0 && at < STATE_PAYLOAD_LEN, ESP_ERR_NO_MEM, TAG,
                        "state payload does not fit");

    size_t included = 0;
    for (size_t i = 0; i < count; i++) {
        const size_t room = STATE_PAYLOAD_LEN - at - 2; // ']' and '}' still to come
        char *cursor = payload + at;

        int step = 0;
        if (included > 0) {
            if (room < 1) {
                break;
            }
            *cursor++ = ',';
            step = 1;
        }

        const int written = append_sample(cursor, room - step, &samples[i], now_s);
        if (written < 0) {
            /* Out of room. Stopping here loses the oldest samples from this
             * payload but keeps the JSON valid and the newest readings in it,
             * which is the right way round. They stay in the buffer. */
            ESP_LOGW(TAG, "bundle truncated at %u of %u samples", (unsigned)included,
                     (unsigned)count);
            break;
        }
        at += step + written;
        included++;
    }

    at += snprintf(payload + at, STATE_PAYLOAD_LEN - at, "]}");
    ESP_RETURN_ON_FALSE(at < STATE_PAYLOAD_LEN, ESP_ERR_NO_MEM, TAG, "payload overflow");

    /* QoS 1, not retained: a soil reading that is an hour old is not worth
     * handing to the next subscriber as if it were current. */
    xEventGroupClearBits(s_events, PUBLISHED_BIT);
    const int id = esp_mqtt_client_publish(s_mqtt, s_topic_state, payload, at, 1, 0);
    ESP_RETURN_ON_FALSE(id >= 0, ESP_FAIL, TAG, "state publish failed");
    s_pending_msg_id = id;

    /* Publishing only queues the message. Returning here would let the caller
     * clear the buffer and go to sleep with the payload still in the client,
     * and the samples would be gone - so wait for the PUBACK. */
    const EventBits_t bits = xEventGroupWaitBits(s_events, PUBLISHED_BIT, pdTRUE, pdTRUE,
                                                 pdMS_TO_TICKS(PUBLISH_ACK_TIMEOUT_MS));
    s_pending_msg_id = -1;
    if ((bits & PUBLISHED_BIT) == 0) {
        ESP_LOGW(TAG, "no PUBACK within %d ms - %u samples stay buffered",
                 PUBLISH_ACK_TIMEOUT_MS, (unsigned)count);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "published %u samples (%d bytes): %s", (unsigned)included, at, payload);
    return ESP_OK;
}

esp_err_t app_net_stop(void)
{
    if (s_mqtt != NULL) {
        /* A clean DISCONNECT rather than just dropping the socket: the broker
         * then knows the station left on purpose. */
        esp_mqtt_client_disconnect(s_mqtt);
        esp_mqtt_client_stop(s_mqtt);
        esp_mqtt_client_destroy(s_mqtt);
        s_mqtt = NULL;
        s_mqtt_connected = false;
    }

    /* Only stop, no deinit: everything about to be freed is in RAM the deep
     * sleep clears anyway, and esp_wifi_stop() is what actually sends the
     * disassociation and turns the radio off. */
    esp_wifi_stop();
    return ESP_OK;
}
