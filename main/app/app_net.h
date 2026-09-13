/* WiFi and MQTT for the application.
 *
 * In `main/` rather than in a component because none of it is board-specific
 * and all of it is policy: which topics, which Home Assistant discovery
 * payload, how long to wait for an association. A component would have to take
 * all of that through an init struct and would be a wrapper around esp_wifi
 * that adds nothing (firmware-architecture.md, "When a wrapper earns its
 * place").
 *
 * With deep sleep the radio comes up once per publish cycle and goes down
 * again: app_net_start(), app_net_publish(), app_net_stop(), sleep. In the
 * bench loop (APP_USE_DEEP_SLEEP 0) it is started once and left up, which is
 * the same three calls with the stop at the end never happening.
 */
#pragma once

#include <stddef.h>

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

/* Publishes one bundle: `samples` oldest first, `count` of them, the newest
 * becoming the entity state and the whole set riding along as a JSON attribute
 * (product design §6.2). Also sends the Home Assistant discovery configuration
 * on the first successful connection after a cold boot.
 *
 * Waits for the broker to acknowledge the message before returning, and only
 * returns ESP_OK if it did. That is what makes it safe for the caller to clear
 * the buffer on success and keep it otherwise: QoS 1 only queues the publish,
 * and a device that sleeps immediately afterwards takes the message with it. */
esp_err_t app_net_publish(const app_sample_t *samples, size_t count);

/* Disconnects MQTT and stops the radio. Called before deep sleep - not to save
 * the microamps of the few milliseconds involved, but so the access point and
 * the broker see the station leave rather than time out. */
esp_err_t app_net_stop(void);
