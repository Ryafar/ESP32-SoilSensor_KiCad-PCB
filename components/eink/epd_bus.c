#include "epd_bus.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd_bus";

/* Frames are 5 kB (1 bpp) or 10 kB (2 bpp) and get sent in pieces this size.
 * The bounce buffer exists so callers may pass a framebuffer from any heap:
 * spi_device_polling_transmit() rejects buffers that are not DMA-capable once
 * DMA is enabled, and a driver that fails depending on where its caller
 * happened to allocate is a trap. Ten memcpys per frame cost nothing next to
 * the seconds the panel spends refreshing. */
#define EPD_BUS_CHUNK 1024

#define EPD_BUS_RAIL_SETTLE_MS 10
#define EPD_BUS_BUSY_POLL_MS 10

struct epd_bus {
    epd_bus_config_t config;
    spi_device_handle_t device;
    uint8_t *bounce;
    bool powered;
};

static inline int rail_level(const struct epd_bus *bus, bool on)
{
    return bus->config.power_active_low ? (on ? 0 : 1) : (on ? 1 : 0);
}

static void delay_ms(int ms)
{
    if (ms <= 0) {
        return;
    }
    /* pdMS_TO_TICKS truncates, so a delay shorter than one tick would not delay
     * at all. Panel reset pulses are specified as minimums. */
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks == 0 ? 1 : ticks);
}

/* Every line except BUSY is an output; BUSY is the only thing the panel ever
 * tells us. Called on each power_on, because power_off hands CLK and MOSI to
 * the SPI driver teardown and then re-takes them as plain GPIOs. */
static esp_err_t configure_control_pins(struct epd_bus *bus)
{
    const gpio_config_t out = {
        .pin_bit_mask = (1ULL << bus->config.pin_cs) | (1ULL << bus->config.pin_dc) |
                        (1ULL << bus->config.pin_rst),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "control pin config failed");

    /* BUSY is pulled towards its *asserted* level on purpose. With no panel on
     * the FPC the pin floats, and a float that happens to read "idle" lets the
     * whole init sequence run against nothing and report success - the worst
     * possible bring-up failure, because the only symptom is a blank panel.
     * Pulled this way, a missing panel looks permanently busy and comes out of
     * epd_bus_wait_idle() as a timeout with a message that says where to look.
     * The panel drives this line push-pull, so the pull never fights it. */
    const gpio_config_t in = {
        .pin_bit_mask = 1ULL << bus->config.pin_busy,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = bus->config.busy_active_level ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en =
            bus->config.busy_active_level ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in), TAG, "busy pin config failed");

    /* CS idles high, RST idles released. D/C is set per transfer. */
    gpio_set_level(bus->config.pin_cs, 1);
    gpio_set_level(bus->config.pin_rst, 1);
    return ESP_OK;
}

static esp_err_t install_spi(struct epd_bus *bus)
{
    const spi_bus_config_t bus_config = {
        .mosi_io_num = bus->config.pin_mosi,
        .miso_io_num = -1, // the panel never answers on SPI
        .sclk_io_num = bus->config.pin_clk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_BUS_CHUNK,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(bus->config.spi_host, &bus_config, SPI_DMA_CH_AUTO),
                        TAG, "spi_bus_initialize failed");

    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = bus->config.clock_hz,
        .mode = 0, // both controllers sample on the rising edge, clock idles low
        .spics_io_num = -1, // CS is driven by hand, see epd_bus.h
        .queue_size = 1,
    };
    const esp_err_t err = spi_bus_add_device(bus->config.spi_host, &device_config, &bus->device);
    if (err != ESP_OK) {
        spi_bus_free(bus->config.spi_host);
        ESP_RETURN_ON_ERROR(err, TAG, "spi_bus_add_device failed");
    }
    return ESP_OK;
}

static void remove_spi(struct epd_bus *bus)
{
    if (bus->device != NULL) {
        spi_bus_remove_device(bus->device);
        bus->device = NULL;
        spi_bus_free(bus->config.spi_host);
    }
}

/* Only meaningful once SPI is gone: while the peripheral owns CLK and MOSI, a
 * gpio_set_level() on them is ignored. */
static void park_lines_low(struct epd_bus *bus)
{
    const gpio_config_t out = {
        .pin_bit_mask = (1ULL << bus->config.pin_clk) | (1ULL << bus->config.pin_mosi) |
                        (1ULL << bus->config.pin_cs) | (1ULL << bus->config.pin_dc) |
                        (1ULL << bus->config.pin_rst),
        .mode = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&out) != ESP_OK) {
        return;
    }
    gpio_set_level(bus->config.pin_clk, 0);
    gpio_set_level(bus->config.pin_mosi, 0);
    gpio_set_level(bus->config.pin_cs, 0);
    gpio_set_level(bus->config.pin_dc, 0);
    gpio_set_level(bus->config.pin_rst, 0);
}

static esp_err_t transmit(struct epd_bus *bus, const uint8_t *data, size_t length)
{
    spi_transaction_t transaction = {
        .length = length * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(bus->device, &transaction);
}

/* Sends length bytes with CS asserted for the whole call. When repeat is true,
 * data points at a single byte that fills the burst. */
static esp_err_t send_bytes(struct epd_bus *bus, int dc_level, const uint8_t *data,
                            size_t length, bool repeat)
{
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_ARG, TAG, "bus is NULL");
    ESP_RETURN_ON_FALSE(bus->powered, ESP_ERR_INVALID_STATE, TAG, "bus is powered down");
    if (length == 0) {
        return ESP_OK;
    }

    gpio_set_level(bus->config.pin_dc, dc_level);
    gpio_set_level(bus->config.pin_cs, 0);

    esp_err_t err = ESP_OK;
    size_t sent = 0;
    if (repeat) {
        memset(bus->bounce, data[0], EPD_BUS_CHUNK);
    }
    while (sent < length) {
        const size_t chunk = (length - sent) < EPD_BUS_CHUNK ? (length - sent) : EPD_BUS_CHUNK;
        if (!repeat) {
            memcpy(bus->bounce, data + sent, chunk);
        }
        err = transmit(bus, bus->bounce, chunk);
        if (err != ESP_OK) {
            break;
        }
        sent += chunk;
    }

    gpio_set_level(bus->config.pin_cs, 1);
    ESP_RETURN_ON_ERROR(err, TAG, "spi transfer failed after %u of %u bytes",
                        (unsigned)sent, (unsigned)length);
    return ESP_OK;
}

esp_err_t epd_bus_new(const epd_bus_config_t *config, epd_bus_handle_t *out_bus)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(out_bus != NULL, ESP_ERR_INVALID_ARG, TAG, "out_bus is NULL");
    ESP_RETURN_ON_FALSE(config->clock_hz > 0, ESP_ERR_INVALID_ARG, TAG, "clock_hz is not set");
    ESP_RETURN_ON_FALSE(config->busy_active_level == 0 || config->busy_active_level == 1,
                        ESP_ERR_INVALID_ARG, TAG, "busy_active_level must be 0 or 1");

    struct epd_bus *bus = calloc(1, sizeof(*bus));
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_NO_MEM, TAG, "out of memory");

    bus->bounce = heap_caps_malloc(EPD_BUS_CHUNK, MALLOC_CAP_DMA);
    if (bus->bounce == NULL) {
        free(bus);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG,
                            "no DMA-capable memory for the bounce buffer");
    }

    bus->config = *config;
    *out_bus = bus;
    return ESP_OK;
}

esp_err_t epd_bus_delete(epd_bus_handle_t bus)
{
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_ARG, TAG, "bus is NULL");

    epd_bus_power_off(bus);
    free(bus->bounce);
    free(bus);
    return ESP_OK;
}

esp_err_t epd_bus_power_on(epd_bus_handle_t bus)
{
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_ARG, TAG, "bus is NULL");
    if (bus->powered) {
        return ESP_OK;
    }

    const gpio_config_t power = {
        .pin_bit_mask = 1ULL << bus->config.pin_power,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&power), TAG, "power pin config failed");
    gpio_set_level(bus->config.pin_power, rail_level(bus, true));
    delay_ms(EPD_BUS_RAIL_SETTLE_MS);

    ESP_RETURN_ON_ERROR(configure_control_pins(bus), TAG, "control pins failed");
    ESP_RETURN_ON_ERROR(install_spi(bus), TAG, "spi install failed");

    bus->powered = true;
    return ESP_OK;
}

esp_err_t epd_bus_power_off(epd_bus_handle_t bus)
{
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_ARG, TAG, "bus is NULL");
    if (!bus->powered) {
        return ESP_OK;
    }

    remove_spi(bus);
    park_lines_low(bus);
    gpio_set_level(bus->config.pin_power, rail_level(bus, false));
    bus->powered = false;
    return ESP_OK;
}

esp_err_t epd_bus_reset(epd_bus_handle_t bus, int high_ms, int low_ms, int settle_ms,
                        int busy_timeout_ms)
{
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_ARG, TAG, "bus is NULL");
    ESP_RETURN_ON_FALSE(bus->powered, ESP_ERR_INVALID_STATE, TAG, "bus is powered down");

    gpio_set_level(bus->config.pin_rst, 1);
    delay_ms(high_ms);
    gpio_set_level(bus->config.pin_rst, 0);
    delay_ms(low_ms);
    gpio_set_level(bus->config.pin_rst, 1);
    delay_ms(settle_ms);

    return epd_bus_wait_idle(bus, busy_timeout_ms);
}

esp_err_t epd_bus_command(epd_bus_handle_t bus, uint8_t command)
{
    return send_bytes(bus, 0, &command, 1, false);
}

esp_err_t epd_bus_command_with_data(epd_bus_handle_t bus, uint8_t command,
                                    const uint8_t *data, size_t length)
{
    ESP_RETURN_ON_ERROR(epd_bus_command(bus, command), TAG, "command 0x%02X failed", command);
    if (length == 0) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "data is NULL");
    return send_bytes(bus, 1, data, length, false);
}

esp_err_t epd_bus_data(epd_bus_handle_t bus, const void *data, size_t length)
{
    ESP_RETURN_ON_FALSE(data != NULL || length == 0, ESP_ERR_INVALID_ARG, TAG, "data is NULL");
    return send_bytes(bus, 1, (const uint8_t *)data, length, false);
}

esp_err_t epd_bus_data_fill(epd_bus_handle_t bus, uint8_t value, size_t length)
{
    return send_bytes(bus, 1, &value, length, true);
}

bool epd_bus_is_busy(epd_bus_handle_t bus)
{
    if (bus == NULL || !bus->powered) {
        return false;
    }
    return gpio_get_level(bus->config.pin_busy) == bus->config.busy_active_level;
}

esp_err_t epd_bus_wait_idle(epd_bus_handle_t bus, int timeout_ms)
{
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_ARG, TAG, "bus is NULL");
    ESP_RETURN_ON_FALSE(bus->powered, ESP_ERR_INVALID_STATE, TAG, "bus is powered down");

    int waited_ms = 0;
    while (epd_bus_is_busy(bus)) {
        if (waited_ms >= timeout_ms) {
            /* Either the panel is not there, BUSY is on the wrong pin, or its
             * polarity is inverted. All three look identical from here, so say
             * so rather than guessing. */
            ESP_LOGE(TAG, "BUSY still asserted after %d ms - check the panel, the BUSY pin "
                          "and its polarity", timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        delay_ms(EPD_BUS_BUSY_POLL_MS);
        waited_ms += EPD_BUS_BUSY_POLL_MS;
    }
    return ESP_OK;
}
