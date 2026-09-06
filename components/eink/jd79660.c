#include "jd79660.h"

#include "esp_check.h"

static const char *TAG = "jd79660";

/* Command set, JD79660 datasheet v1.0.3. Only what this driver uses. */
#define CMD_PANEL_SETTING 0x00
#define CMD_POWER_OFF 0x02
#define CMD_POWER_ON 0x04
#define CMD_BOOSTER_SOFT_START 0x06
#define CMD_DEEP_SLEEP 0x07
#define CMD_DISPLAY_START_TRANSMISSION 0x10
#define CMD_DISPLAY_REFRESH 0x12
#define CMD_VCOM_DATA_INTERVAL 0x50
#define CMD_RESOLUTION_SETTING 0x61
#define CMD_PARTIAL_WINDOW 0x83
#define CMD_CASCADE_SETTING 0x4D
#define CMD_PLL_UNDOCUMENTED 0x30
#define CMD_FAST_MODE_ENABLE 0xE0
#define CMD_FAST_MODE_TRIGGER 0xA5
#define CMD_FAST_MODE_VALUE 0xE6
#define CMD_TEMP_UNDOCUMENTED 0xE9

/* Reset: the datasheet asks for 20 ms high, 40 ms low and 50 ms of settling.
 * Good Display's demo runs 2 ms pulses and works, but nothing here is timing
 * critical - this happens once per wake - so the specified numbers are used. */
#define RESET_HIGH_MS 20
#define RESET_LOW_MS 40
#define RESET_SETTLE_MS 50

/* A refresh measures ~21 s. The timeout is generous for the same reason as on
 * the black/white part: it catches a missing panel, not a cold one. */
#define BUSY_TIMEOUT_RESET_MS 2000
#define BUSY_TIMEOUT_POWER_MS 2000
#define BUSY_TIMEOUT_REFRESH_MS 40000

/* Data interval / border settings written to 0x50. The border is held white
 * during a refresh; the alternative is a border that slowly picks up colour. */
#define VCOM_INTERVAL_WHITE_BORDER 0x37

static epd_bus_handle_t s_bus;

/* Aims the transmission at the whole panel. The controller can address a
 * sub-window, but a partial window buys nothing here: this part has no
 * differential waveform, so any refresh drives the full ~20 s regardless. */
static esp_err_t set_full_window(void)
{
    const uint8_t window[] = {
        0x00, 0x00,                                              // x start
        (JD79660_WIDTH - 1) >> 8, (JD79660_WIDTH - 1) & 0xFF,    // x end
        0x00, 0x00,                                              // y start
        (JD79660_HEIGHT - 1) >> 8, (JD79660_HEIGHT - 1) & 0xFF,  // y end
        0x00,                                                    // whole-screen mode
    };
    return epd_bus_command_with_data(s_bus, CMD_PARTIAL_WINDOW, window, sizeof(window));
}

static esp_err_t power_on(void)
{
    ESP_RETURN_ON_ERROR(epd_bus_command(s_bus, CMD_POWER_ON), TAG, "power on failed");
    return epd_bus_wait_idle(s_bus, BUSY_TIMEOUT_POWER_MS);
}

static esp_err_t power_off(void)
{
    const uint8_t argument[] = {0x00};
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_POWER_OFF, argument, 1), TAG,
                        "power off failed");
    return epd_bus_wait_idle(s_bus, BUSY_TIMEOUT_POWER_MS);
}

static esp_err_t refresh(void)
{
    const uint8_t interval[] = {VCOM_INTERVAL_WHITE_BORDER};
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_VCOM_DATA_INTERVAL, interval, 1),
                        TAG, "vcom interval failed");

    const uint8_t argument[] = {0x00};
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_DISPLAY_REFRESH, argument, 1), TAG,
                        "display refresh failed");
    return epd_bus_wait_idle(s_bus, BUSY_TIMEOUT_REFRESH_MS);
}

esp_err_t jd79660_init(epd_bus_handle_t bus)
{
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_ARG, TAG, "bus is NULL");

    s_bus = bus;
    ESP_RETURN_ON_ERROR(epd_bus_power_on(s_bus), TAG, "panel rail failed to come up");
    ESP_RETURN_ON_ERROR(epd_bus_reset(s_bus, RESET_HIGH_MS, RESET_LOW_MS, RESET_SETTLE_MS,
                                      BUSY_TIMEOUT_RESET_MS),
                        TAG, "reset failed");

    /* Cascade setting. Single-chip panel, so this only disables the cascade
     * handshake; the value is the one the vendor demo uses. */
    const uint8_t cascade[] = {0x78};
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_CASCADE_SETTING, cascade, 1), TAG,
                        "cascade setting failed");

    /* Panel setting: four-colour mode, scan and shift directions for this glass,
     * booster on, soft reset released. */
    const uint8_t panel[] = {0x0F, 0x29};
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_PANEL_SETTING, panel, 2), TAG,
                        "panel setting failed");

    /* Booster soft start. These are drive strengths and ramp times matched to
     * the module's 47 uH inductor - not values to improvise on. */
    const uint8_t booster[] = {0x0D, 0x12, 0x30, 0x20, 0x19, 0x2A, 0x22};
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_BOOSTER_SOFT_START, booster,
                                                  sizeof(booster)),
                        TAG, "booster soft start failed");

    const uint8_t interval[] = {VCOM_INTERVAL_WHITE_BORDER};
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_VCOM_DATA_INTERVAL, interval, 1),
                        TAG, "vcom interval failed");

    const uint8_t resolution[] = {JD79660_WIDTH >> 8, JD79660_WIDTH & 0xFF, JD79660_HEIGHT >> 8,
                                  JD79660_HEIGHT & 0xFF};
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_RESOLUTION_SETTING, resolution, 4),
                        TAG, "resolution setting failed");

    /* Two registers the datasheet does not document but the vendor demo sets on
     * every init. Left in, and named for what they are, rather than dropped on
     * the theory that undocumented means unnecessary. */
    const uint8_t temp[] = {0x01};
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_TEMP_UNDOCUMENTED, temp, 1), TAG,
                        "0xE9 failed");
    const uint8_t pll[] = {0x08};
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_PLL_UNDOCUMENTED, pll, 1), TAG,
                        "0x30 failed");

    /* Fast mode. Cuts the refresh from ~30 s to ~21 s at the cost of the low
     * end of the temperature range. A soil sensor that sits outdoors in winter
     * may want this off; that is a call for whoever fields the product, so it
     * is one #define rather than buried in the sequence. */
#if CONFIG_JD79660_FAST_MODE
    const uint8_t fast_enable[] = {0x02};
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_FAST_MODE_ENABLE, fast_enable, 1),
                        TAG, "fast mode enable failed");
    const uint8_t fast_value[] = {0x5D};
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_FAST_MODE_VALUE, fast_value, 1), TAG,
                        "fast mode value failed");
    const uint8_t fast_trigger[] = {0x00};
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_FAST_MODE_TRIGGER, fast_trigger, 1),
                        TAG, "fast mode trigger failed");
    ESP_RETURN_ON_ERROR(epd_bus_wait_idle(s_bus, BUSY_TIMEOUT_POWER_MS), TAG,
                        "controller did not settle after fast mode");
#endif

    ESP_RETURN_ON_ERROR(power_on(), TAG, "panel supply failed to come up");

    ESP_LOGI(TAG, "JD79660 ready, %dx%d, 4 colours", JD79660_WIDTH, JD79660_HEIGHT);
    return ESP_OK;
}

esp_err_t jd79660_refresh(const uint8_t *frame)
{
    ESP_RETURN_ON_FALSE(s_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(frame != NULL, ESP_ERR_INVALID_ARG, TAG, "frame is NULL");

    ESP_RETURN_ON_ERROR(set_full_window(), TAG, "window failed");
    ESP_RETURN_ON_ERROR(epd_bus_command(s_bus, CMD_DISPLAY_START_TRANSMISSION), TAG,
                        "start transmission failed");
    ESP_RETURN_ON_ERROR(epd_bus_data(s_bus, frame, JD79660_FRAME_BYTES), TAG, "frame write failed");
    return refresh();
}

esp_err_t jd79660_fill_screen(jd79660_color_t color)
{
    ESP_RETURN_ON_FALSE(s_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    ESP_RETURN_ON_ERROR(set_full_window(), TAG, "window failed");
    ESP_RETURN_ON_ERROR(epd_bus_command(s_bus, CMD_DISPLAY_START_TRANSMISSION), TAG,
                        "start transmission failed");
    ESP_RETURN_ON_ERROR(epd_bus_data_fill(s_bus, JD79660_SOLID_BYTE(color), JD79660_FRAME_BYTES),
                        TAG, "fill failed");
    return refresh();
}

esp_err_t jd79660_sleep(void)
{
    ESP_RETURN_ON_FALSE(s_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    /* Order matters: the charge pump has to be down before the controller stops
     * listening, otherwise it keeps running on a chip that can no longer be
     * told to stop. */
    ESP_RETURN_ON_ERROR(power_off(), TAG, "panel supply failed to go down");

    const uint8_t check_code[] = {0xA5}; // the command is ignored without it
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_DEEP_SLEEP, check_code, 1), TAG,
                        "deep sleep failed");

    s_bus = NULL;
    return ESP_OK;
}
