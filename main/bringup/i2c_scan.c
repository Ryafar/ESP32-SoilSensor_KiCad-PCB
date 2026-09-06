#include "i2c_scan.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

static const char *TAG = "i2c_scan";

#define ADDR_FIRST 0x08
#define ADDR_LAST 0x77
#define PROBE_TIMEOUT_MS 50

/* Half a bit period at ~100 kHz. */
#define CLOCK_HALF_PERIOD_US 5
#define RECOVERY_CLOCKS 9

/* Long enough that even a badly overdamped line has finished, short enough not
 * to stall the boot. At 100 kHz a bit lasts 10 us, so anything above a few
 * microseconds is already a problem. */
#define RISE_TIMEOUT_US 20000

static void release_as_input(int pin)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
}

/* Open drain with the pull-up on, which is how an I2C pin has to behave: we
 * can only pull low, never drive high. */
static void configure_as_open_drain(int pin)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    gpio_set_level(pin, 1);
}

/* Waits for a released line to reach a high level. Returns the time it took in
 * microseconds, or -1 if it never got there. */
static int64_t wait_for_high(int pin)
{
    const int64_t start = esp_timer_get_time();

    while (gpio_get_level(pin) == 0) {
        if (esp_timer_get_time() - start > RISE_TIMEOUT_US) {
            return -1;
        }
    }
    return esp_timer_get_time() - start;
}

/* Pulls the line low, confirms it actually went low, releases it and measures
 * how long the pull-up needs to bring it back. */
static void characterise_line(int pin, const char *name)
{
    gpio_set_level(pin, 0);
    esp_rom_delay_us(50);

    if (gpio_get_level(pin) != 0) {
        ESP_LOGE(TAG, "%s(GPIO%d): cannot be driven low - the pin is not "
                      "connected to what we think it is", name, pin);
        gpio_set_level(pin, 1);
        return;
    }

    gpio_set_level(pin, 1);
    const int64_t rise_us = wait_for_high(pin);

    if (rise_us < 0) {
        ESP_LOGE(TAG, "%s(GPIO%d): never returns high - held low externally",
                 name, pin);
    } else if (rise_us > 100) {
        ESP_LOGW(TAG, "%s(GPIO%d): rise time %lld us - far too slow for I2C. "
                      "Pull-up too weak or too much capacitance on the line",
                 name, pin, rise_us);
    } else {
        ESP_LOGI(TAG, "%s(GPIO%d): rise time %lld us", name, pin, rise_us);
    }
}

void i2c_report_line_levels(int sda, int scl)
{
    release_as_input(sda);
    release_as_input(scl);
    esp_rom_delay_us(1000); // let the internal pull-ups win

    const int sda_idle = gpio_get_level(sda);
    const int scl_idle = gpio_get_level(scl);

    ESP_LOGI(TAG, "idle levels: SDA(GPIO%d)=%d SCL(GPIO%d)=%d", sda, sda_idle,
             scl, scl_idle);

    if (sda_idle == 0 || scl_idle == 0) {
        ESP_LOGE(TAG, "a line is stuck low - no transfer can start. Suspects: "
                      "slave holding the bus, short to GND, or wrong pin");
        return;
    }

    configure_as_open_drain(sda);
    configure_as_open_drain(scl);

    /* How fast can each line recover on its own? This separates "shorted" from
     * "merely too slow", which look identical if you only sample once. */
    characterise_line(sda, "SDA");
    characterise_line(scl, "SCL");

    /* Cross-talk, both directions reported separately. A solder bridge between
     * the two lines is symmetric; a one-sided result is something else. */
    gpio_set_level(sda, 0);
    esp_rom_delay_us(200);
    const int scl_while_sda_low = gpio_get_level(scl);
    gpio_set_level(sda, 1);
    wait_for_high(sda);

    gpio_set_level(scl, 0);
    esp_rom_delay_us(200);
    const int sda_while_scl_low = gpio_get_level(sda);
    gpio_set_level(scl, 1);
    wait_for_high(scl);

    ESP_LOGI(TAG, "cross-talk: SCL=%d while SDA low, SDA=%d while SCL low "
                  "(both should be 1)", scl_while_sda_low, sda_while_scl_low);

    if (scl_while_sda_low == 0 && sda_while_scl_low == 0) {
        ESP_LOGE(TAG, "both directions follow - the two lines are shorted together");
    } else if (scl_while_sda_low == 0 || sda_while_scl_low == 0) {
        ESP_LOGW(TAG, "only one direction follows - not a plain short. More "
                      "likely a slow line that had not recovered yet, or a "
                      "device coupling the two");
    } else {
        ESP_LOGI(TAG, "lines idle high and independent");
    }

    release_as_input(sda);
    release_as_input(scl);
}

bool i2c_bus_recover(int sda, int scl)
{
    configure_as_open_drain(sda);
    configure_as_open_drain(scl);

    if (gpio_get_level(sda) == 1) {
        ESP_LOGI(TAG, "SDA already high, no recovery needed");
        release_as_input(sda);
        release_as_input(scl);
        return true;
    }

    ESP_LOGW(TAG, "SDA held low, clocking the bus free");
    for (int i = 0; i < RECOVERY_CLOCKS && gpio_get_level(sda) == 0; i++) {
        gpio_set_level(scl, 0);
        esp_rom_delay_us(CLOCK_HALF_PERIOD_US);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(CLOCK_HALF_PERIOD_US);
    }

    /* STOP condition: SDA goes high while SCL is high. */
    gpio_set_level(sda, 0);
    esp_rom_delay_us(CLOCK_HALF_PERIOD_US);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(CLOCK_HALF_PERIOD_US);
    gpio_set_level(sda, 1);
    esp_rom_delay_us(CLOCK_HALF_PERIOD_US);

    const bool recovered = gpio_get_level(sda) == 1;
    ESP_LOGW(TAG, "recovery %s", recovered ? "succeeded" : "failed, SDA still low");

    release_as_input(sda);
    release_as_input(scl);
    return recovered;
}

void i2c_scan_bus(i2c_master_bus_handle_t bus, const char *label)
{
    int found = 0;

    for (uint16_t addr = ADDR_FIRST; addr <= ADDR_LAST; addr++) {
        if (i2c_master_probe(bus, addr, PROBE_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGI(TAG, "%s: device at 0x%02X", label, addr);
            found++;
        }
    }

    if (found == 0) {
        ESP_LOGW(TAG, "%s: nothing responded", label);
    } else {
        ESP_LOGI(TAG, "%s: %d device(s)", label, found);
    }
}

static void scan_once(int sda, int scl, const char *label)
{
    const i2c_master_bus_config_t config = {
        .i2c_port = -1,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus = NULL;
    const esp_err_t err = i2c_new_master_bus(&config, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: cannot create bus: %s", label, esp_err_to_name(err));
        return;
    }

    i2c_scan_bus(bus, label);
    i2c_del_master_bus(bus);
}

void i2c_scan_pin_orientations(int sda, int scl)
{
    char label[48];

    snprintf(label, sizeof(label), "SDA=%d SCL=%d as configured", sda, scl);
    scan_once(sda, scl, label);

    snprintf(label, sizeof(label), "SDA=%d SCL=%d swapped", scl, sda);
    scan_once(scl, sda, label);
}
