/* WiFi and MQTT for the application.
 *
 * In `main/` rather than in a component because none of it is board-specific
 * and all of it is policy: which topics, which Home Assistant discovery
 * payload, how long to wait for an association. A component would have to take
 * all of that through an init struct and would be a wrapper around esp_wifi
 * that adds nothing (firmware-architecture.md, "When a wrapper earns its
 * place").
 *
 * This stage keeps the radio up between measurements, which is exactly what the
 * final firmware must not do - there the radio comes up once per publish cycle
 * and goes down before deep sleep. Bench convenience now, and the split between
 * app_net_start() and app_net_publish() is already where that change will go.
 */
#pragma once

#include "esp_err.h"

#include "app_sample.h"

/* Brings up NVS, the event loop, the station interface and the MQTT client, and
 * waits for an IP address. Returns ESP_ERR_TIMEOUT if the association does not
 * complete - the caller is expected to keep measuring anyway, because a
 * measurement that reaches the log is worth more than one that is skipped
 * because the network is down. */
esp_err_t app_net_start(void);

/* True once MQTT is connected. Publishing before that is not an error, it just
 * does nothing. */
bool app_net_is_connected(void);

/* Publishes the sample as the entity state, plus the Home Assistant discovery
 * configuration on the first successful connection. */
esp_err_t app_net_publish(const app_sample_t *sample);
