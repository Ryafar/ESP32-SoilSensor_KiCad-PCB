/* PLACEHOLDER - see eink.h. Nothing here talks to hardware yet.
 *
 * The implementation is blocked on identifying the panel's controller IC. Every
 * function fails loudly rather than silently doing nothing, so a caller wired
 * up against this component cannot mistake "not implemented" for "worked".
 */
#include "eink.h"

#include "esp_check.h"

static const char *TAG = "eink";

static bool s_configured;

esp_err_t eink_init(const eink_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");

    ESP_LOGW(TAG, "placeholder driver: panel controller not identified yet");
    ESP_LOGW(TAG, "would drive %dx%d on SPI%d, CS=%d DC=%d RST=%d BUSY=%d POWER=%d",
             config->width, config->height, config->spi_host + 1, config->pin_cs,
             config->pin_dc, config->pin_rst, config->pin_busy, config->pin_power);

    s_configured = true;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t eink_render(const eink_screen_t *screen)
{
    ESP_RETURN_ON_FALSE(screen != NULL, ESP_ERR_INVALID_ARG, TAG, "screen is NULL");

    ESP_LOGW(TAG, "placeholder render: soil %d%%, %d mV, charging=%d, low=%d",
             screen->soil_percent, screen->battery_mv, screen->charging,
             screen->battery_low);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t eink_render_full(const eink_screen_t *screen)
{
    return eink_render(screen);
}

bool eink_is_busy(void)
{
    return false;
}

esp_err_t eink_power_off(void)
{
    if (!s_configured) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "placeholder power off: panel rail not actually switched");
    return ESP_ERR_NOT_SUPPORTED;
}
