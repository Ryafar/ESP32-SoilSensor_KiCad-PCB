/* The samples taken since the last successful publish.
 *
 * Measuring is cheap and happens every wake; the radio is expensive and runs
 * every sixth. The five readings in between have to live somewhere, and that
 * somewhere is RTC memory - free to write, no flash wear, and losing it on a
 * power cut costs nothing worth protecting (product design §4).
 *
 * Not NVS. Writing a sample to a key-value store every five minutes uses it
 * against its purpose and spends flash endurance on data that is about to be
 * transmitted anyway.
 *
 * Sized at APP_BUFFER_SLOTS, which is two hours of samples at the design
 * interval, so a broker that is down for an hour costs nothing. On overflow the
 * oldest sample is dropped: recent soil moisture is worth more than old soil
 * moisture. The drop is counted rather than silent - an unexplained gap in a
 * graph is worse than a number saying how many readings were lost.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "app_sample.h"

#define APP_BUFFER_SLOTS 24

/* Appends a sample, dropping the oldest if the buffer is full. */
void app_buffer_add(const app_sample_t *sample);

/* Oldest first. Returns NULL past the end. */
const app_sample_t *app_buffer_at(size_t index);

size_t app_buffer_count(void);

/* How many samples the buffer had to drop since the last clear. Published
 * alongside the data so a gap has an explanation. */
uint32_t app_buffer_dropped(void);

/* Called only after the broker has acknowledged the samples. Everything else -
 * a failed association, a refused connection, a missing PUBACK - leaves the
 * buffer alone so the next cycle sends more rather than less. */
void app_buffer_clear(void);
