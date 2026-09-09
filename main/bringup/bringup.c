/* Bring-up build: shared resources, drivers and the hardware checks on top.
 *
 * This is app_main for the throwaway half of the firmware. It creates what the
 * checks share - the I2C bus and the ADC unit - initialises the drivers they
 * talk through, and starts the tasks. Stack sizes and priorities are decided
 * here on purpose: scheduling policy belongs in one visible place, not
 * scattered across the modules that happen to need a task.
 *
 * Shared buses are owned here as well. The I2C bus is created once and lent to
 * the drivers that sit on it, so no driver can claim a peripheral another one
 * is already using.
 *
 * Nothing in main/app/ may include this file. Which half runs is decided in
 * main.c and nowhere else, so the checks can be rewired without the
 * application noticing and vice versa.
 */
#include "bringup.h"

#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "battery_test_task.h"
#include "bq25186.h"
#include "charger_test_task.h"
#include "config.h"
#include "eink_test_task.h"
#include "i2c_scan.h"
#include "rgb.h"
#include "rgb_test_task.h"
#include "soil_test_task.h"
#include "wifi_test_task.h"

static const char *TAG = "bringup";

#define RGB_TEST_TASK_STACK 3072
#define RGB_TEST_TASK_PRIO 3
#define CHARGER_TEST_TASK_STACK 3072
#define CHARGER_TEST_TASK_PRIO 3
/* The panel driver blocks on BUSY rather than recursing, so the stack only has
 * to carry the init sequence and the log calls. The framebuffer is on the
 * heap. */
#define EINK_TEST_TASK_STACK 4096
#define EINK_TEST_TASK_PRIO 3
#define BATTERY_TEST_TASK_STACK 4096
#define BATTERY_TEST_TASK_PRIO 3
/* The scan blocks in this task and the results are sorted and formatted here,
 * on top of whatever the WiFi driver charges to the caller. */
#define WIFI_TEST_TASK_STACK 4096
#define WIFI_TEST_TASK_PRIO 3
#define SOIL_TEST_TASK_STACK 4096
#define SOIL_TEST_TASK_PRIO 3

/* Which bring-up tasks actually start. Only the one under test needs to run -
 * the others just fill the log, and a failing task nobody is looking at
 * (the e-ink panel, or the battery with no cell attached) trains you to ignore
 * errors. Kept as switches rather than commented-out xTaskCreate calls so there
 * is one place that says what is live. */
#define RUN_RGB_TEST     0
#define RUN_CHARGER_TEST 1
#define RUN_EINK_TEST    1
#define RUN_BATTERY_TEST 0  // needs a cell attached, otherwise it only logs -1
#define RUN_SOIL_TEST    0
/* Rail toggle instead of the measurement loop: drives SOIL_POWER (GPIO14) up
 * and down every two seconds so a meter on CON2 can confirm the AO3401A
 * switches at all. Mutually exclusive with RUN_SOIL_TEST - both own the pin. */
#define RUN_SOIL_RAIL_TOGGLE 1
#define RUN_WIFI_TEST    1

#define I2C_TIMEOUT_MS 100

static i2c_master_bus_handle_t s_i2c_bus;
static adc_oneshot_unit_handle_t s_adc;

/* Outlive the tasks that read them - a stack local would be gone by the time
 * the task runs. Guarded so a disabled task does not leave an unused static
 * behind. */
#if RUN_BATTERY_TEST
static battery_test_config_t s_battery_test_cfg;
#endif
#if RUN_SOIL_TEST || RUN_SOIL_RAIL_TOGGLE
static soil_test_config_t s_soil_test_cfg;
#endif

static esp_err_t init_i2c(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = -1, // let the driver pick a free port
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* Harmless if the board already has external pull-ups, and the
         * difference between "no pull-ups" and "chip missing" is otherwise an
         * afternoon of debugging. */
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_config, &s_i2c_bus);
}

/* ADC1 is shared: battery sensing reads GPIO0, soil sampling will read GPIO1.
 * The unit can only be created once, so this file owns it and lends the
 * handle. */
static esp_err_t init_adc(void)
{
    const adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    return adc_oneshot_new_unit(&init_config, &s_adc);
}

void bringup_run(void)
{
    ESP_LOGI(TAG, "soil sensor firmware, bring-up build");

    const rgb_config_t rgb_cfg = {
        .gpio_num = PIN_RGB_DIN,
        .brightness = 16,
    };
    ESP_ERROR_CHECK(rgb_init(&rgb_cfg));

    /* Bring-up diagnostics, cheapest question first: can the lines sit high,
     * is a slave holding the bus, and only then is anyone home. All of this
     * runs before the real bus exists and leaves nothing behind. */
    i2c_report_line_levels(PIN_I2C_SDA, PIN_I2C_SCL);
    i2c_bus_recover(PIN_I2C_SDA, PIN_I2C_SCL);
    i2c_scan_pin_orientations(PIN_I2C_SDA, PIN_I2C_SCL);

    ESP_ERROR_CHECK(init_i2c());

    const bq25186_config_t charger_cfg = {
        .bus = s_i2c_bus,
        .device_address = BQ25186_I2C_ADDR,
        .scl_speed_hz = I2C_CLK_HZ,
        .timeout_ms = I2C_TIMEOUT_MS,
    };
    ESP_ERROR_CHECK(bq25186_init(&charger_cfg));

    /* The ADC unit stays unconditional: both the battery and the soil task
     * borrow it, and creating it costs nothing when neither runs. */
    ESP_ERROR_CHECK(init_adc());

#if RUN_RGB_TEST
    xTaskCreate(rgb_test_task, "rgb_test", RGB_TEST_TASK_STACK, NULL,
                RGB_TEST_TASK_PRIO, NULL);
#endif

#if RUN_CHARGER_TEST
    xTaskCreate(charger_test_task, "charger_test", CHARGER_TEST_TASK_STACK, NULL,
                CHARGER_TEST_TASK_PRIO, NULL);
#endif

#if RUN_EINK_TEST
    /* The e-ink task owns the panel end to end - it initialises it, draws, and
     * puts it back to sleep - because a refresh blocks for seconds and nothing
     * else in this build has a reason to wait that long. */
    xTaskCreate(eink_test_task, "eink_test", EINK_TEST_TASK_STACK, NULL,
                EINK_TEST_TASK_PRIO, NULL);
#endif

#if RUN_BATTERY_TEST
    s_battery_test_cfg = (battery_test_config_t){
        .adc = s_adc,
        .meas_gpio = PIN_BAT_MEAS,
        .enable_gpio = PIN_BAT_MEAS_EN,
        .int_gpio = PIN_CHARGER_INT,
        .power_good_gpio = PIN_POWER_GOOD,
        .divider_ratio = BATTERY_DIVIDER_RATIO,
    };
    xTaskCreate(battery_test_task, "battery_test", BATTERY_TEST_TASK_STACK,
                &s_battery_test_cfg, BATTERY_TEST_TASK_PRIO, NULL);
#endif

#if RUN_SOIL_TEST && RUN_SOIL_RAIL_TOGGLE
#error "RUN_SOIL_TEST and RUN_SOIL_RAIL_TOGGLE both drive SOIL_POWER - pick one"
#endif

#if RUN_SOIL_TEST || RUN_SOIL_RAIL_TOGGLE
    s_soil_test_cfg = (soil_test_config_t){
        .adc = s_adc,
        .meas_gpio = PIN_SOIL_MEAS,
        .power_gpio = PIN_SOIL_POWER,
        .power_active_low = SOIL_POWER_ACTIVE_LOW,
    };
#endif
#if RUN_SOIL_TEST
    xTaskCreate(soil_test_task, "soil_test", SOIL_TEST_TASK_STACK,
                &s_soil_test_cfg, SOIL_TEST_TASK_PRIO, NULL);
#endif
#if RUN_SOIL_RAIL_TOGGLE
    xTaskCreate(soil_rail_toggle_task, "soil_toggle", SOIL_TEST_TASK_STACK,
                &s_soil_test_cfg, SOIL_TEST_TASK_PRIO, NULL);
#endif

#if RUN_WIFI_TEST
    /* Owns the radio end to end, the same way the e-ink task owns the panel -
     * nothing else in this build touches WiFi. */
    xTaskCreate(wifi_test_task, "wifi_test", WIFI_TEST_TASK_STACK, NULL,
                WIFI_TEST_TASK_PRIO, NULL);
#endif

    /* Returns to app_main; the created tasks keep running. */
}
