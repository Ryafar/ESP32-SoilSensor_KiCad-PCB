#include "app_buffer.h"

#include <inttypes.h>

#include "esp_attr.h"
#include "esp_log.h"

static const char *TAG = "app_buffer";

/* All of it survives deep sleep; none of it survives a power cut, which is the
 * trade §4 of the product design picks deliberately. A plain array with a head
 * index rather than a head/tail pair: the buffer is only ever appended to and
 * cleared as a whole, so one index and a count say everything. */
static RTC_DATA_ATTR app_sample_t s_samples[APP_BUFFER_SLOTS];
static RTC_DATA_ATTR size_t s_count;
static RTC_DATA_ATTR size_t s_oldest;
static RTC_DATA_ATTR uint32_t s_dropped;

void app_buffer_add(const app_sample_t *sample)
{
    if (sample == NULL) {
        return;
    }

    if (s_count == APP_BUFFER_SLOTS) {
        /* Full: overwrite the oldest slot and move the window on. Logged at
         * warning level because by the time this happens the device has been
         * unable to reach the broker for two hours. */
        s_samples[s_oldest] = *sample;
        s_oldest = (s_oldest + 1) % APP_BUFFER_SLOTS;
        s_dropped++;
        ESP_LOGW(TAG, "buffer full - dropped the oldest sample (%" PRIu32 " so far)",
                 s_dropped);
        return;
    }

    s_samples[(s_oldest + s_count) % APP_BUFFER_SLOTS] = *sample;
    s_count++;
}

const app_sample_t *app_buffer_at(size_t index)
{
    if (index >= s_count) {
        return NULL;
    }
    return &s_samples[(s_oldest + index) % APP_BUFFER_SLOTS];
}

size_t app_buffer_count(void)
{
    return s_count;
}

uint32_t app_buffer_dropped(void)
{
    return s_dropped;
}

void app_buffer_clear(void)
{
    s_count = 0;
    s_oldest = 0;
    s_dropped = 0;
}
