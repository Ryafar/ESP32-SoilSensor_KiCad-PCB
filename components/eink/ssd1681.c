#include "ssd1681.h"

#include "esp_check.h"

static const char *TAG = "ssd1681";

/* Command set, SSD1681 datasheet section 6. Only what this driver uses. */
#define CMD_DRIVER_OUTPUT_CONTROL 0x01
#define CMD_DEEP_SLEEP_MODE 0x10
#define CMD_DATA_ENTRY_MODE 0x11
#define CMD_SW_RESET 0x12
#define CMD_TEMP_SENSOR_CONTROL 0x18
#define CMD_MASTER_ACTIVATION 0x20
#define CMD_DISPLAY_UPDATE_CONTROL_2 0x22
#define CMD_WRITE_RAM_NEW 0x24 // the image that will be displayed
#define CMD_WRITE_RAM_OLD 0x26 // the image the differential waveform starts from
#define CMD_BORDER_WAVEFORM 0x3C
#define CMD_SET_RAM_X_RANGE 0x44
#define CMD_SET_RAM_Y_RANGE 0x45
#define CMD_SET_RAM_X_COUNTER 0x4E
#define CMD_SET_RAM_Y_COUNTER 0x4F

/* Display update sequence selectors written to 0x22. The bit fields enable the
 * clock, the analog supply, the display stage and the shutdown - the two
 * constants differ only in which waveform stage runs. */
#define UPDATE_SEQUENCE_FULL 0xF7
#define UPDATE_SEQUENCE_PARTIAL 0xFC

/* Reset pulse: the datasheet asks for 10 ms, and the controller then needs its
 * own settling time before it will answer. */
#define RESET_HIGH_MS 10
#define RESET_LOW_MS 10
#define RESET_SETTLE_MS 10

/* A full refresh measures ~2.5 s and a partial one ~0.5 s. The timeouts are
 * deliberately several times that: they exist to catch a panel that is absent
 * or miswired, not to police a panel that is merely cold and therefore slow. */
#define BUSY_TIMEOUT_RESET_MS 1000
#define BUSY_TIMEOUT_FULL_MS 10000
#define BUSY_TIMEOUT_PARTIAL_MS 5000

#define ROW_BYTES (SSD1681_WIDTH / 8)

static epd_bus_handle_t s_bus;

/* Points the RAM window at the whole panel and rewinds the address counters.
 * Every RAM write starts from here; the controller auto-increments through the
 * window as bytes arrive, so the frame goes out as one flat burst. */
static esp_err_t set_full_ram_window(void)
{
    const uint8_t entry_mode[] = {0x03}; // X increment, Y increment
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_DATA_ENTRY_MODE, entry_mode, 1),
                        TAG, "data entry mode failed");

    /* X is the packed axis, so it is addressed in bytes: 0 .. 24 for 200 px. */
    const uint8_t x_range[] = {0x00, ROW_BYTES - 1};
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_SET_RAM_X_RANGE, x_range, 2),
                        TAG, "RAM X range failed");

    /* Y is a line count and takes two bytes per endpoint, low byte first. */
    const uint8_t y_range[] = {0x00, 0x00, (SSD1681_HEIGHT - 1) & 0xFF,
                               (SSD1681_HEIGHT - 1) >> 8};
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_SET_RAM_Y_RANGE, y_range, 4),
                        TAG, "RAM Y range failed");

    const uint8_t x_counter[] = {0x00};
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_SET_RAM_X_COUNTER, x_counter, 1),
                        TAG, "RAM X counter failed");

    const uint8_t y_counter[] = {0x00, 0x00};
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_SET_RAM_Y_COUNTER, y_counter, 2),
                        TAG, "RAM Y counter failed");
    return ESP_OK;
}

static esp_err_t write_ram(uint8_t command, const uint8_t *frame)
{
    ESP_RETURN_ON_ERROR(set_full_ram_window(), TAG, "RAM window failed");
    ESP_RETURN_ON_ERROR(epd_bus_command(s_bus, command), TAG, "command 0x%02X failed", command);
    return epd_bus_data(s_bus, frame, SSD1681_FRAME_BYTES);
}

static esp_err_t fill_ram(uint8_t command, uint8_t value)
{
    ESP_RETURN_ON_ERROR(set_full_ram_window(), TAG, "RAM window failed");
    ESP_RETURN_ON_ERROR(epd_bus_command(s_bus, command), TAG, "command 0x%02X failed", command);
    return epd_bus_data_fill(s_bus, value, SSD1681_FRAME_BYTES);
}

static esp_err_t run_update(uint8_t sequence, int timeout_ms)
{
    const uint8_t selector[] = {sequence};
    ESP_RETURN_ON_ERROR(
        epd_bus_command_with_data(s_bus, CMD_DISPLAY_UPDATE_CONTROL_2, selector, 1), TAG,
        "update control failed");
    ESP_RETURN_ON_ERROR(epd_bus_command(s_bus, CMD_MASTER_ACTIVATION), TAG,
                        "master activation failed");
    return epd_bus_wait_idle(s_bus, timeout_ms);
}

esp_err_t ssd1681_init(epd_bus_handle_t bus)
{
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_ARG, TAG, "bus is NULL");

    s_bus = bus;
    ESP_RETURN_ON_ERROR(epd_bus_power_on(s_bus), TAG, "panel rail failed to come up");
    ESP_RETURN_ON_ERROR(epd_bus_reset(s_bus, RESET_HIGH_MS, RESET_LOW_MS, RESET_SETTLE_MS,
                                      BUSY_TIMEOUT_RESET_MS),
                        TAG, "reset failed");

    /* The software reset repeats what the pin already did, but it is what the
     * datasheet documents as the entry point to the init sequence and it clears
     * state a marginal power-up may have left behind. */
    ESP_RETURN_ON_ERROR(epd_bus_command(s_bus, CMD_SW_RESET), TAG, "soft reset failed");
    ESP_RETURN_ON_ERROR(epd_bus_wait_idle(s_bus, BUSY_TIMEOUT_RESET_MS), TAG,
                        "controller did not come back from soft reset");

    /* Number of gate lines as (count-1), low byte first, then the scan options.
     * 199 = 0xC7 for this panel. */
    const uint8_t driver_output[] = {(SSD1681_HEIGHT - 1) & 0xFF, (SSD1681_HEIGHT - 1) >> 8, 0x00};
    ESP_RETURN_ON_ERROR(
        epd_bus_command_with_data(s_bus, CMD_DRIVER_OUTPUT_CONTROL, driver_output, 3), TAG,
        "driver output control failed");

    /* Border waveform. Follows the LUT for the border pixels, which keeps the
     * one-pixel frame around the image from drifting grey over many refreshes. */
    const uint8_t border[] = {0x05};
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_BORDER_WAVEFORM, border, 1), TAG,
                        "border waveform failed");

    /* Use the on-chip temperature sensor to pick the waveform. E-paper is
     * strongly temperature dependent and this board has no other sensor to
     * offer, so the alternative is a waveform for the wrong temperature. */
    const uint8_t temp_sensor[] = {0x80};
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_TEMP_SENSOR_CONTROL, temp_sensor, 1),
                        TAG, "temperature sensor select failed");

    ESP_RETURN_ON_ERROR(set_full_ram_window(), TAG, "RAM window failed");

    ESP_LOGI(TAG, "SSD1681 ready, %dx%d", SSD1681_WIDTH, SSD1681_HEIGHT);
    return ESP_OK;
}

esp_err_t ssd1681_refresh_full(const uint8_t *frame)
{
    ESP_RETURN_ON_FALSE(s_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(frame != NULL, ESP_ERR_INVALID_ARG, TAG, "frame is NULL");

    /* Old bank first, new bank second. The full waveform ignores the old bank,
     * but writing it here means the banks agree once the refresh is done, which
     * is the precondition the next partial refresh needs. */
    ESP_RETURN_ON_ERROR(write_ram(CMD_WRITE_RAM_OLD, frame), TAG, "old RAM write failed");
    ESP_RETURN_ON_ERROR(write_ram(CMD_WRITE_RAM_NEW, frame), TAG, "new RAM write failed");
    return run_update(UPDATE_SEQUENCE_FULL, BUSY_TIMEOUT_FULL_MS);
}

esp_err_t ssd1681_refresh_partial(const uint8_t *frame)
{
    ESP_RETURN_ON_FALSE(s_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(frame != NULL, ESP_ERR_INVALID_ARG, TAG, "frame is NULL");

    ESP_RETURN_ON_ERROR(write_ram(CMD_WRITE_RAM_NEW, frame), TAG, "new RAM write failed");
    ESP_RETURN_ON_ERROR(run_update(UPDATE_SEQUENCE_PARTIAL, BUSY_TIMEOUT_PARTIAL_MS), TAG,
                        "partial update failed");

    /* Copy into the old bank only now. Doing it before the refresh would make
     * the difference empty and nothing on the glass would move. */
    return write_ram(CMD_WRITE_RAM_OLD, frame);
}

esp_err_t ssd1681_fill_screen(bool white)
{
    ESP_RETURN_ON_FALSE(s_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    const uint8_t value = white ? 0xFF : 0x00; // a set bit is a white pixel
    ESP_RETURN_ON_ERROR(fill_ram(CMD_WRITE_RAM_OLD, value), TAG, "old RAM fill failed");
    ESP_RETURN_ON_ERROR(fill_ram(CMD_WRITE_RAM_NEW, value), TAG, "new RAM fill failed");
    return run_update(UPDATE_SEQUENCE_FULL, BUSY_TIMEOUT_FULL_MS);
}

esp_err_t ssd1681_sleep(void)
{
    ESP_RETURN_ON_FALSE(s_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    const uint8_t mode[] = {0x01}; // deep sleep mode 1, RAM contents retained
    ESP_RETURN_ON_ERROR(epd_bus_command_with_data(s_bus, CMD_DEEP_SLEEP_MODE, mode, 1), TAG,
                        "deep sleep failed");

    /* The controller now answers nothing until it is reset, so the handle is
     * dropped: any further call fails as "not initialized" rather than sending
     * bytes into a chip that has stopped listening. */
    s_bus = NULL;
    return ESP_OK;
}
