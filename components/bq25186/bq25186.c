#include "bq25186.h"

#include "driver/gpio.h"
#include "esp_check.h"

static const char *TAG = "bq25186";

/* ICHG encoding, from the datasheet formula for the fast charge current:
 *
 *   code  0 .. 30   ICHG =  5 + code          mA   (1 mA steps,  5 .. 35 mA)
 *   code 31 .. 127  ICHG = 40 + (code-31)*10  mA   (10 mA steps, 40 .. 1000 mA)
 *
 * Two independent checks that this is right: the register's reset value 0x05
 * lands on 10 mA, which is what the datasheet documents as the default, and
 * the top code 127 lands on 1000 mA, which is what the part is rated for. */
#define ICHG_CODE_STEP_BOUNDARY 31
#define ICHG_LOW_RANGE_OFFSET_MA 5
#define ICHG_HIGH_RANGE_BASE_MA 40

static i2c_master_dev_handle_t s_dev;
static int s_timeout_ms;
static int s_ce_gpio = -1; // -1: the firmware does not drive /CE

esp_err_t bq25186_init(const bq25186_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(config->bus != NULL, ESP_ERR_INVALID_ARG, TAG, "bus is NULL");
    ESP_RETURN_ON_FALSE(s_dev == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialized");

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = config->device_address,
        .scl_speed_hz = config->scl_speed_hz,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(config->bus, &dev_config, &s_dev),
                        TAG, "failed to add device at 0x%02X", config->device_address);

    s_timeout_ms = config->timeout_ms;

    if (config->drive_ce_pin) {
        const gpio_config_t ce_config = {
            .pin_bit_mask = 1ULL << config->ce_gpio,
            /* INPUT_OUTPUT, so gpio_get_level reports the pad rather than
             * echoing the last write - with a 10 kOhm pullup on the net that is
             * the difference between "we drive it" and "we think we do". */
            .mode = GPIO_MODE_INPUT_OUTPUT,
            .intr_type = GPIO_INTR_DISABLE,
        };
        /* A pad left holding its level from before a deep sleep ignores
         * gpio_config(), so the hold is released first - otherwise /CE is
         * frozen at whatever the previous cycle chose. */
        gpio_hold_dis(config->ce_gpio);
        ESP_RETURN_ON_ERROR(gpio_config(&ce_config), TAG, "/CE gpio config failed");
        s_ce_gpio = config->ce_gpio;

        /* Charging off until someone asks for it. Same level the board's pullup
         * produces, so this changes nothing electrically - it just makes the
         * state driven and therefore knowable. */
        ESP_RETURN_ON_ERROR(bq25186_set_ce_pin(false), TAG, "/CE initial level failed");
    }

    return ESP_OK;
}

esp_err_t bq25186_set_ce_pin(bool enabled)
{
    ESP_RETURN_ON_FALSE(s_ce_gpio >= 0, ESP_ERR_INVALID_STATE, TAG,
                        "this config does not drive /CE");

    /* /CE is active low: low enables charging. */
    return gpio_set_level(s_ce_gpio, enabled ? 0 : 1);
}

esp_err_t bq25186_ce_pin_hold(bool hold)
{
    ESP_RETURN_ON_FALSE(s_ce_gpio >= 0, ESP_ERR_INVALID_STATE, TAG,
                        "this config does not drive /CE");

    return hold ? gpio_hold_en(s_ce_gpio) : gpio_hold_dis(s_ce_gpio);
}

esp_err_t bq25186_deinit(void)
{
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    ESP_RETURN_ON_ERROR(i2c_master_bus_rm_device(s_dev), TAG, "failed to remove device");
    s_dev = NULL;

    /* The pin keeps whatever level it was driven to - deliberately. Only the
     * application knows whether charging should continue while it sleeps, and
     * silently switching it off here would be a policy decision taken in a
     * driver. It just stops being ours to set. */
    s_ce_gpio = -1;
    return ESP_OK;
}

esp_err_t bq25186_read_reg(uint8_t reg, uint8_t *value)
{
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(value != NULL, ESP_ERR_INVALID_ARG, TAG, "value is NULL");

    return i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, s_timeout_ms);
}

esp_err_t bq25186_write_reg(uint8_t reg, uint8_t value)
{
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    const uint8_t frame[2] = {reg, value};
    return i2c_master_transmit(s_dev, frame, sizeof(frame), s_timeout_ms);
}

esp_err_t bq25186_update_reg(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current;
    ESP_RETURN_ON_ERROR(bq25186_read_reg(reg, &current), TAG, "read 0x%02X failed", reg);

    const uint8_t updated = (uint8_t)((current & ~mask) | (value & mask));
    if (updated == current) {
        return ESP_OK;
    }
    return bq25186_write_reg(reg, updated);
}

esp_err_t bq25186_probe(void)
{
    uint8_t stat0;
    ESP_RETURN_ON_ERROR(bq25186_read_reg(BQ25186_REG_STAT0, &stat0), TAG,
                        "no response - check wiring and address");
    ESP_LOGI(TAG, "charger responded, STAT0 = 0x%02X", stat0);
    return ESP_OK;
}

esp_err_t bq25186_dump_registers(void)
{
    for (uint8_t reg = BQ25186_REG_FIRST; reg <= BQ25186_REG_LAST; reg++) {
        uint8_t value;
        ESP_RETURN_ON_ERROR(bq25186_read_reg(reg, &value), TAG, "read 0x%02X failed", reg);
        ESP_LOGI(TAG, "reg 0x%02X = 0x%02X", reg, value);
    }
    return ESP_OK;
}

static esp_err_t ichg_ma_to_code(uint16_t milliamps, uint8_t *code)
{
    if (milliamps < BQ25186_ICHG_MIN_MA || milliamps > BQ25186_ICHG_MAX_MA) {
        return ESP_ERR_INVALID_ARG;
    }
    if (milliamps <= 35) {
        *code = (uint8_t)(milliamps - ICHG_LOW_RANGE_OFFSET_MA);
        return ESP_OK;
    }
    /* The encoding has no codes between 35 mA and 40 mA, and above 40 mA only
     * 10 mA steps exist. Reject rather than round - a charge current that is
     * quietly not what was asked for is worth an error. */
    if (milliamps < ICHG_HIGH_RANGE_BASE_MA ||
        (milliamps - ICHG_HIGH_RANGE_BASE_MA) % 10 != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *code = (uint8_t)(ICHG_CODE_STEP_BOUNDARY + (milliamps - ICHG_HIGH_RANGE_BASE_MA) / 10);
    return ESP_OK;
}

static uint16_t ichg_code_to_ma(uint8_t code)
{
    if (code < ICHG_CODE_STEP_BOUNDARY) {
        return (uint16_t)(code + ICHG_LOW_RANGE_OFFSET_MA);
    }
    return (uint16_t)(ICHG_HIGH_RANGE_BASE_MA +
                      (code - ICHG_CODE_STEP_BOUNDARY) * 10);
}

esp_err_t bq25186_set_charge_current(uint16_t milliamps)
{
    uint8_t code;
    ESP_RETURN_ON_ERROR(ichg_ma_to_code(milliamps, &code), TAG,
                        "%u mA is not encodable", milliamps);

    /* Writing the whole register also clears CHG_DIS, which is what the caller
     * wants when setting a charge current. */
    ESP_RETURN_ON_ERROR(bq25186_write_reg(BQ25186_REG_ICHG_CTRL, code), TAG,
                        "failed to set charge current");
    ESP_LOGI(TAG, "charge current set to %u mA (code 0x%02X)", milliamps, code);
    return ESP_OK;
}

esp_err_t bq25186_get_charge_current(uint16_t *milliamps)
{
    ESP_RETURN_ON_FALSE(milliamps != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

    uint8_t value;
    ESP_RETURN_ON_ERROR(bq25186_read_reg(BQ25186_REG_ICHG_CTRL, &value), TAG, "read failed");
    *milliamps = ichg_code_to_ma(value & BQ25186_ICHG_CTRL_ICHG_MASK);
    return ESP_OK;
}

esp_err_t bq25186_set_charging_enabled(bool enabled)
{
    return bq25186_update_reg(BQ25186_REG_ICHG_CTRL,
                              BQ25186_ICHG_CTRL_CHG_DIS_MASK,
                              enabled ? 0 : BQ25186_ICHG_CTRL_CHG_DIS_MASK);
}

esp_err_t bq25186_read_status(bq25186_status_t *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "status is NULL");

    ESP_RETURN_ON_ERROR(bq25186_read_reg(BQ25186_REG_STAT0, &status->stat0), TAG, "STAT0");
    ESP_RETURN_ON_ERROR(bq25186_read_reg(BQ25186_REG_STAT1, &status->stat1), TAG, "STAT1");
    /* FLAG0 latches faults and clears on read, so it is read last and its value
     * is only valid for this one call. */
    ESP_RETURN_ON_ERROR(bq25186_read_reg(BQ25186_REG_FLAG0, &status->flag0), TAG, "FLAG0");
    return ESP_OK;
}

esp_err_t bq25186_read_charge_state(bq25186_charge_state_t *state, bool *vin_good)
{
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_INVALID_ARG, TAG, "state is NULL");

    uint8_t stat0 = 0;
    /* STAT0 only, not bq25186_read_status(): that also reads FLAG0, which
     * clears on read, and a caller asking "is it charging" would silently eat
     * the fault flags. */
    ESP_RETURN_ON_ERROR(bq25186_read_reg(BQ25186_REG_STAT0, &stat0), TAG, "STAT0");

    *state = (bq25186_charge_state_t)((stat0 & BQ25186_STAT0_CHG_STAT_MASK) >>
                                      BQ25186_STAT0_CHG_STAT_SHIFT);
    if (vin_good != NULL) {
        *vin_good = (stat0 & BQ25186_STAT0_VIN_PGOOD_MASK) != 0;
    }
    return ESP_OK;
}

bool bq25186_charge_state_is_charging(bq25186_charge_state_t state)
{
    return state == BQ25186_CHARGE_CONSTANT_CURRENT ||
           state == BQ25186_CHARGE_CONSTANT_VOLTAGE;
}

const char *bq25186_charge_state_name(bq25186_charge_state_t state)
{
    switch (state) {
    case BQ25186_CHARGE_IDLE:
        return "idle";
    case BQ25186_CHARGE_CONSTANT_CURRENT:
        return "constant current";
    case BQ25186_CHARGE_CONSTANT_VOLTAGE:
        return "constant voltage";
    case BQ25186_CHARGE_DONE_OR_DISABLED:
        return "done or disabled";
    }
    return "unknown";
}

esp_err_t bq25186_enter_ship_mode(void)
{
    ESP_LOGW(TAG, "entering ship mode - system rail goes away now");

    /* No read-modify-write and no logging after this point: the BATFET opens
     * while the write is being acknowledged, so anything queued behind it is
     * lost. Whether execution continues past this line is up to how fast the
     * rail collapses, not up to this code. */
    return bq25186_update_reg(BQ25186_REG_SHIP_RST,
                              BQ25186_SHIP_RST_EN_RST_SHIP_MASK,
                              BQ25186_SHIP_RST_EN_RST_SHIP_SHIP);
}
