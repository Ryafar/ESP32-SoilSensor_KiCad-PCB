/* The application: measure, log, report.
 *
 * This is stage one of what docs/product-design.md §2 describes, with deep
 * sleep deliberately left out. The final firmware has no loop at all - it wakes,
 * runs the sequence once, and sleeps, and RAM is cleared in between. Here the
 * same sequence runs in a task on a vTaskDelay, so readings arrive in the
 * terminal every minute instead of every half hour and MQTT can be watched
 * while it works.
 *
 * The sequence is kept in one function for exactly that reason: measure_once()
 * is what the wake handler will call, unchanged, once power management lands.
 * The only thing that has to move then is where the radio comes up.
 *
 * What is not implemented yet, and is not pretended to be: ship mode on a flat
 * cell (see APP_ALLOW_SHIP_MODE), the RTC ring buffer and bundled publishing
 * (§6.2 says publishing every cycle is the right first implementation), and
 * reading the calibration from NVS.
 */
#include "app_run.h"

#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_display.h"
#include "app_net.h"
#include "app_sample.h"
#include "battery.h"
#include "bq25186.h"
#include "config.h"
#include "rgb.h"
#include "soil.h"

static const char *TAG = "app";

/* The panel init sequence, the canvas calls and the MQTT publish all run in
 * this one task. The framebuffer itself is on the heap. */
#define MEASURE_TASK_STACK 8192
#define MEASURE_TASK_PRIO 3

#define I2C_TIMEOUT_MS 100

/* Ship mode is the plan for a flat cell (product design §3.2), and it is not
 * armed while the firmware is being developed on a bench: it opens the BATFET,
 * and the only ways back are USB or a long press on TS/MR. A threshold bug
 * would therefore shut the board down every time it was powered from a cell.
 * The branch is still taken and logged, so the decision can be read in the
 * log - it just does not execute. */
#define APP_ALLOW_SHIP_MODE 0

/* 0 keeps the radio down: measure and log, publish nothing. For looking at
 * readings on a bench with no broker reachable, where the reconnect attempt
 * otherwise prints an error every few seconds and buries the numbers.
 *
 * This is a switch about the device's own behaviour - not a per-sensor switch.
 * Which measurements happen is decided by the sequence in §2, from the cell
 * voltage and USB; isolating a single sensor is what main/bringup/ is for. */
#define APP_ENABLE_NETWORK 1

/* 0 leaves the e-paper panel alone. A refresh blocks the measure task for
 * 2.5 s on the black/white panel and around 21 s on the four-colour one, which
 * is a long time to wait when the thing being debugged is a sensor reading.
 *
 * Which panel is fitted is a separate, build-time choice and lives in
 * components/eink/include/eink_panel.h. */
#define APP_ENABLE_DISPLAY 1

/* What the sequence decided this cycle. Logged by name, because "which branch
 * did it take" is the first question about any reading. */
typedef enum {
    CYCLE_MEASURE = 0,  // cell is fine, take a reading
    CYCLE_MEASURE_NO_CELL, // cell unreadable but USB is feeding the board: measure anyway
    CYCLE_CHARGE_WAIT,  // low but on USB: wait, measure nothing
    CYCLE_LOW_BATTERY,  // low and no USB: ship mode
    CYCLE_NO_BATTERY,   // cell unreadable and no USB either: measure nothing
} cycle_decision_t;

static i2c_master_bus_handle_t s_i2c_bus;
static adc_oneshot_unit_handle_t s_adc;
static uint32_t s_measurements;

static const char *decision_name(cycle_decision_t decision)
{
    switch (decision) {
    case CYCLE_MEASURE:
        return "MEASURE";
    case CYCLE_MEASURE_NO_CELL:
        return "MEASURE (no cell, on USB)";
    case CYCLE_CHARGE_WAIT:
        return "CHARGE WAIT";
    case CYCLE_LOW_BATTERY:
        return "LOW BATTERY";
    case CYCLE_NO_BATTERY:
        return "NO BATTERY READING";
    }
    return "?";
}

static esp_err_t init_i2c(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = -1, // let the driver pick a free port
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_config, &s_i2c_bus);
}

/* ADC1 is shared: the battery divider is on GPIO0, the soil probe on GPIO1.
 * The unit can only be created once, so this file owns it and lends the
 * handle to both drivers. */
static esp_err_t init_adc(void)
{
    const adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    return adc_oneshot_new_unit(&init_config, &s_adc);
}

/* LED1 is not used by anything yet, so the only thing worth doing with it is
 * making sure it is off - as a driven level, not as a hope about what the ROM
 * left on the pad. GPIO17 is UART0 RX by default and can come up with a
 * pull-up, which would leak through the LED for the rest of the cell's life.
 * See config.h for the arithmetic. */
static esp_err_t init_status_led_off(void)
{
    const gpio_config_t led_config = {
        .pin_bit_mask = 1ULL << PIN_STATUS_LED,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&led_config), TAG, "status led config failed");
    return gpio_set_level(PIN_STATUS_LED, STATUS_LED_ACTIVE_HIGH ? 0 : 1);
}

static esp_err_t init_drivers(void)
{
    ESP_RETURN_ON_ERROR(init_status_led_off(), TAG, "status led off failed");

    const rgb_config_t rgb_cfg = {
        .gpio_num = PIN_RGB_DIN,
        .brightness = 16,
    };
    ESP_RETURN_ON_ERROR(rgb_init(&rgb_cfg), TAG, "rgb init failed");
    rgb_set_status(RGB_STATUS_BOOT);

    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "i2c bus failed");

    const bq25186_config_t charger_cfg = {
        .bus = s_i2c_bus,
        .device_address = BQ25186_I2C_ADDR,
        .scl_speed_hz = I2C_CLK_HZ,
        .timeout_ms = I2C_TIMEOUT_MS,
        /* The board's pull-up holds /CE high, which disables charging, so this
         * pin is not optional here - without it the cell never charges. */
        .ce_gpio = PIN_CHARGE_EN,
        .drive_ce_pin = true,
    };
    ESP_RETURN_ON_ERROR(bq25186_init(&charger_cfg), TAG, "charger init failed");
    ESP_RETURN_ON_ERROR(bq25186_probe(), TAG,
                        "the charger does not answer on the bus - nothing below "
                        "this can be trusted");

    ESP_RETURN_ON_ERROR(init_adc(), TAG, "adc unit failed");

    const battery_config_t battery_cfg = {
        .adc = s_adc,
        .meas_gpio = PIN_BAT_MEAS,
        .enable_gpio = PIN_BAT_MEAS_EN,
        .int_gpio = PIN_CHARGER_INT,
        .power_good_gpio = PIN_POWER_GOOD,
        .enable_active_high = BAT_MEAS_EN_ACTIVE_HIGH,
        .divider_ratio = BATTERY_DIVIDER_RATIO,
        .settle_ms = BATTERY_SETTLE_MS,
        .samples = BATTERY_ADC_SAMPLES,
    };
    ESP_RETURN_ON_ERROR(battery_init(&battery_cfg), TAG, "battery init failed");

    const soil_config_t soil_cfg = {
        .adc = s_adc,
        .meas_gpio = PIN_SOIL_MEAS,
        .power_gpio = PIN_SOIL_POWER,
        .power_active_low = SOIL_POWER_ACTIVE_LOW,
        .settle_ms = SOIL_SETTLE_MS,
        .samples = SOIL_ADC_SAMPLES,
        .dry_mv = SOIL_CALIBRATION_DRY_MV,
        .wet_mv = SOIL_CALIBRATION_WET_MV,
    };
    ESP_RETURN_ON_ERROR(soil_init(&soil_cfg), TAG, "soil init failed");

    return ESP_OK;
}

/* The charger's watchdog reverts every register to its defaults after 160 s
 * without an I2C transaction, so the charge current is rewritten every cycle
 * rather than once at boot (product design §3.3). Configuration here is
 * stateless by design, not by oversight. */
static void configure_charger(void)
{
    const esp_err_t err = bq25186_set_charge_current(BATTERY_CHARGE_CURRENT_MA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not set the charge current: %s", esp_err_to_name(err));
    }
}

/* The /CE level currently driven, or -1 before anything has been driven this
 * boot. Kept so the log gets one line when charging starts or stops, instead of
 * one every cycle saying the same thing. */
static int s_charging_allowed = -1;

/* Returns true when the level actually changed. */
static bool set_charging_allowed(bool allowed)
{
    if (s_charging_allowed == (int)allowed) {
        return false;
    }

    const esp_err_t err = bq25186_set_ce_pin(allowed);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not drive /CE: %s", esp_err_to_name(err));
        return false;
    }

    s_charging_allowed = allowed;
    ESP_LOGI(TAG, "VIN %s -> charging %s (/CE driven %s)",
             allowed ? "present" : "gone", allowed ? "enabled" : "disabled",
             allowed ? "low" : "high");
    return true;
}

/* Wrapper so the switch appears once instead of at every call site. */
static void update_display(const app_sample_t *sample, bool battery_low)
{
#if APP_ENABLE_DISPLAY
    app_display_update(sample, battery_low);
#else
    (void)sample;
    (void)battery_low;
#endif
}

static cycle_decision_t decide(const battery_reading_t *battery, bool battery_valid,
                               bool usb_present)
{
    if (!battery_valid) {
        /* No cell fitted, or the divider did not switch in. On USB that is the
         * normal bench case - the board is fed from VIN, so the soil rail and
         * the radio are not drawing from a cell whose voltage is unknown, and
         * skipping the measurement would mean a board on a desk that never
         * measures anything.
         *
         * On battery alone it is the opposite: the one thing the thresholds
         * exist to prevent is loading a cell that might be flat. */
        return usb_present ? CYCLE_MEASURE_NO_CELL : CYCLE_NO_BATTERY;
    }
    if (battery->millivolts >= BATTERY_RECOVER_MV) {
        return CYCLE_MEASURE;
    }
    if (battery->millivolts < BATTERY_SHIP_MODE_MV) {
        /* Never ship mode while USB is present: it opens the BATFET and would
         * strand a device that is already being rescued (§3.2). */
        return battery->usb_present ? CYCLE_CHARGE_WAIT : CYCLE_LOW_BATTERY;
    }
    /* Between the two thresholds: coming up from empty means waiting, and a
     * cell on its way down keeps measuring until it crosses the lower one. */
    return battery->usb_present ? CYCLE_CHARGE_WAIT : CYCLE_MEASURE;
}

static void measure_once(void)
{
    app_sample_t sample = {
        .sequence = ++s_measurements,
        .soil_mv = -1,
        .soil_percent = -1,
        .battery_mv = -1,
    };

    rgb_set_status(RGB_STATUS_MEASURING);
    configure_charger();

    bq25186_charge_state_t charge_state = BQ25186_CHARGE_IDLE;
    bool charger_vin_good = false;
    esp_err_t charge_err = bq25186_read_charge_state(&charge_state, &charger_vin_good);
    if (charge_err != ESP_OK) {
        ESP_LOGW(TAG, "could not read the charger state: %s", esp_err_to_name(charge_err));
    }

    /* Charging is allowed exactly while VIN is there, and the decision comes
     * from the charger's own VIN_PGOOD bit rather than from the board's PGOOD
     * pin - the chip is the thing whose opinion about its input matters.
     *
     * Both directions are load-bearing. Low is what makes the cell charge at
     * all on this board, because the 10 kOhm pull-up would otherwise hold /CE
     * high. High is what keeps that pull-up from burning ~330 uA out of the
     * cell once USB is gone, which is 33x the entire sleep budget. /CE has no
     * effect without VIN, so this is purely about that current. */
    if (charge_err == ESP_OK && set_charging_allowed(charger_vin_good)) {
        /* Re-read after a change: CHG_STAT sampled before the pin moved
         * describes the previous state, and reporting "not charging" on the
         * cycle where charging just started is the kind of off-by-one-cycle
         * that gets debugged twice. */
        charge_err = bq25186_read_charge_state(&charge_state, &charger_vin_good);
    }
    sample.charging = bq25186_charge_state_is_charging(charge_state);

    /* Battery first: every other branch depends on the answer, and a
     * measurement taken on a collapsing rail is worse than no measurement
     * (product design §2). */
    battery_reading_t battery = {0};
    const esp_err_t battery_err = battery_read(&battery);
    const bool battery_valid = battery_err == ESP_OK;
    if (battery_valid) {
        sample.battery_mv = battery.millivolts;
    } else {
        ESP_LOGW(TAG, "battery reading failed: %s", esp_err_to_name(battery_err));
    }
    sample.usb_present = battery_valid ? battery.usb_present : charger_vin_good;

    /* The charger's own VIN bit and the board's PGOOD pin should agree. When
     * they do not, one of them is not measuring what it is thought to measure -
     * worth a line rather than a silent preference for either. */
    if (battery_valid && charge_err == ESP_OK && battery.usb_present != charger_vin_good) {
        ESP_LOGW(TAG, "PGOOD pin says USB %s but the charger says %s",
                 battery.usb_present ? "present" : "absent",
                 charger_vin_good ? "present" : "absent");
    }

    const cycle_decision_t decision = decide(&battery, battery_valid, sample.usb_present);

    if (decision == CYCLE_MEASURE || decision == CYCLE_MEASURE_NO_CELL) {
        soil_reading_t soil = {0};
        const esp_err_t soil_err = soil_read(&soil);
        if (soil_err == ESP_OK) {
            sample.soil_mv = soil.millivolts;
            sample.soil_percent = soil.percent;
        } else {
            ESP_LOGW(TAG, "soil reading failed: %s", esp_err_to_name(soil_err));
        }
    }

    /* One line with everything, so scrolling the log is enough to follow what
     * the device did and why. */
    ESP_LOGI(TAG, "#%" PRIu32 " %s  soil=%d mV (%d %%)  vbat=%d mV  usb=%d  charger=%s",
             sample.sequence, decision_name(decision), sample.soil_mv,
             sample.soil_percent, sample.battery_mv, sample.usb_present,
             bq25186_charge_state_name(charge_state));

    if (decision == CYCLE_LOW_BATTERY) {
        rgb_set_status(RGB_STATUS_BATTERY_LOW);

        /* The screen goes up before ship mode, not after: ship mode opens the
         * BATFET and there is no "after" (product design §2). Whatever is on
         * the glass when the rail dies is what someone finds days later, so it
         * has to be the low-battery frame. */
        update_display(&sample, true);
#if APP_ALLOW_SHIP_MODE
        /* Nothing after this line executes: the BATFET opens while the write is
         * still being acknowledged. */
        bq25186_enter_ship_mode();
#else
        ESP_LOGW(TAG, "cell is below %d mV with no USB - the final firmware "
                      "enters ship mode here; APP_ALLOW_SHIP_MODE is 0",
                 BATTERY_SHIP_MODE_MV);
#endif
        return;
    }

#if APP_ENABLE_NETWORK
    if (sample.sequence % PUBLISH_EVERY_N_MEASUREMENTS == 0) {
        const esp_err_t err = app_net_publish(&sample);
        /* ESP_ERR_INVALID_STATE is "not connected", which app_net has already
         * logged - no second line for the same fact. */
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "publish failed: %s", esp_err_to_name(err));
        }
    }
#endif

    /* Last, and after the publish, because it is the only step that can be
     * skipped without losing data - and because it is by far the slowest
     * (product design §2). Redraws only when the picture would actually
     * differ; app_display decides that. */
    update_display(&sample, false);

    /* The LED says what the board is doing between measurements. It costs real
     * current and comes off the budget once deep sleep lands - for bench work
     * it is the fastest way to see the state from across the room. */
    rgb_set_status(sample.charging ? RGB_STATUS_CHARGING : RGB_STATUS_OFF);
}

static void measure_task(void *arg)
{
    while (1) {
        measure_once();
        vTaskDelay(pdMS_TO_TICKS(MEASUREMENT_INTERVAL_S * 1000));
    }
}

void app_run(void)
{
    ESP_LOGI(TAG, "soil sensor firmware, application build");
    ESP_LOGI(TAG, "measuring every %d s, publishing every %d measurements, "
                  "charge current %d mA",
             MEASUREMENT_INTERVAL_S, PUBLISH_EVERY_N_MEASUREMENTS,
             BATTERY_CHARGE_CURRENT_MA);

    const esp_err_t err = init_drivers();
    if (err != ESP_OK) {
        /* Deliberately not ESP_ERROR_CHECK: that aborts, abort means reboot,
         * and a reboot loop on a battery is a flat cell in a few days with
         * nobody watching (firmware-architecture.md §5). Fault colour, and the
         * log says what failed. */
        ESP_LOGE(TAG, "driver bring-up failed: %s - not measuring",
                 esp_err_to_name(err));
        rgb_set_status(RGB_STATUS_FAULT);
        return;
    }

#if APP_ENABLE_NETWORK
    /* Before the first measurement, so the first sample can be published.
     * Failing to associate is not fatal: measurements still reach the log, and
     * the radio keeps trying in the background. */
    const esp_err_t net_err = app_net_start();
    if (net_err != ESP_OK) {
        ESP_LOGW(TAG, "network not up yet (%s) - measuring anyway",
                 esp_err_to_name(net_err));
    }
#else
    ESP_LOGW(TAG, "APP_ENABLE_NETWORK is 0 - radio stays down, nothing is "
                  "published, readings go to the log only");
#endif

    xTaskCreate(measure_task, "measure", MEASURE_TASK_STACK, NULL, MEASURE_TASK_PRIO,
                NULL);

    /* Returns; FreeRTOS deletes the main task and the measure task keeps
     * running. */
}
