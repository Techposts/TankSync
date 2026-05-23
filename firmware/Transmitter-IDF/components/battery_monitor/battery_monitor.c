/**
 * battery_monitor — INA219-only implementation (simplified 2026-05-16)
 *
 * Single measurement path: INA219 over I²C at 0x40. NVS key "pwr_mode_ovr"
 * (namespace "system") accepts "auto" / "ina219" / "disabled". If INA219 is
 * not present at boot, mode falls to POWER_MODE_NONE (no fallback — voltage
 * divider mode removed). TX continues running with zero power telemetry.
 *
 * Sign convention (per wiring: IN+ on battery+, IN- on load):
 *   current_ma > 0  → current OUT of battery (discharging)
 *   current_ma < 0  → current INTO battery (charging)
 *   charging flag is simply (current_ma < 0).
 */

#include "battery_monitor.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "power";

#ifndef INA219_I2C_ADDR
#define INA219_I2C_ADDR             0x40
#endif
#ifndef INA219_I2C_FREQ_HZ
#define INA219_I2C_FREQ_HZ          100000
#endif
#ifndef INA219_PROBE_TIMEOUT_MS
#define INA219_PROBE_TIMEOUT_MS     50
#endif
#ifndef INA219_READ_TIMEOUT_MS
#define INA219_READ_TIMEOUT_MS      100
#endif
// 0.1 Ω shunt + 10 µV LSB → current_mA = shunt_raw / 10 (signed).
#define INA219_SHUNT_DIV_PER_MA     10

#define NVS_NS_PWR              "system"
#define NVS_KEY_PWR_OVERRIDE    "pwr_mode_ovr"
#define DEFAULT_OVERRIDE        "auto"
#define MAX_OVERRIDE_LEN        12

// INA219 registers used
#define INA219_REG_CONFIG       0x00
#define INA219_REG_SHUNT_V      0x01
#define INA219_REG_BUS_V        0x02

// Configuration register value chosen for TankSync TX workload:
//   bit 15    RST   = 0  (no reset)
//   bit 14    —     = 0  (reserved)
//   bit 13    BRNG  = 0  (16V bus range — battery max ~4.2V)
//   bit 12-11 PG    = 10 (÷4 shunt gain, 160 mV full-scale = 1.6 A max)
//   bit 10-7  BADC  = 1100 (12-bit, 16-sample averaging, ~8.5 ms per bus V conversion)
//   bit 6-3   SADC  = 1100 (12-bit, 16-sample averaging, ~8.5 ms per shunt V conversion)
//   bit 2-0   MODE  = 111  (continuous shunt + bus)
// Total: 0001 0110 0110 0111 = 0x1667
// Averaging cuts noise by √16 = 4× vs single-sample default, at the cost of
// ~17ms total conversion latency per read (acceptable for our wake cycle).
#define INA219_CONFIG_VALUE     0x1667

// ── Module state ─────────────────────────────────────────────────────────────
static power_mode_t              s_mode       = POWER_MODE_NONE;
static i2c_master_bus_handle_t   s_i2c_bus    = NULL;
static i2c_master_dev_handle_t   s_ina219_dev = NULL;

// ── LiPo 12-point discharge curve (replaces old 3-point linear) ──────────────
// Li-ion discharge voltage is non-linear: the 3.6-4.0V band represents
// 30-80% of usable capacity but a 3-point linear curve squashed it,
// causing 15-20% SoC error in the dangerous low-battery range (3.3-3.6V).
// This 12-point table follows typical NMC 18650 chemistry under light load.
// Linear interpolation between points keeps the math simple while tracking
// the curve's flat region correctly.
static int mv_to_pct(uint32_t mv) {
    static const struct { uint16_t mv; uint8_t pct; } curve[] = {
        { 4200, 100 },
        { 4100,  92 },
        { 4000,  82 },
        { 3900,  70 },
        { 3800,  58 },
        { 3700,  45 },
        { 3600,  32 },
        { 3500,  20 },
        { 3400,  10 },
        { 3300,   5 },
        { 3200,   2 },
        { 3000,   0 },
    };
    const int N = (int)(sizeof(curve) / sizeof(curve[0]));
    if (mv >= curve[0].mv)     return 100;
    if (mv <= curve[N-1].mv)   return 0;
    for (int i = 1; i < N; i++) {
        if (mv >= curve[i].mv) {
            uint32_t span_mv  = curve[i-1].mv  - curve[i].mv;
            uint32_t span_pct = curve[i-1].pct - curve[i].pct;
            uint32_t delta_mv = mv - curve[i].mv;
            return (int)(curve[i].pct + (delta_mv * span_pct) / span_mv);
        }
    }
    return 0;
}

// ── I²C bus + INA219 handle ──────────────────────────────────────────────────
static esp_err_t i2c_bus_init(int sda_pin, int scl_pin) {
    if (s_i2c_bus) return ESP_OK;  // idempotent
    if (sda_pin < 0 || scl_pin < 0) return ESP_ERR_INVALID_ARG;

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1,
        .scl_io_num = scl_pin,
        .sda_io_num = sda_pin,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I²C bus init failed: %s", esp_err_to_name(err));
        s_i2c_bus = NULL;
        return err;
    }
    ESP_LOGI(TAG, "I²C bus init OK (SDA=%d, SCL=%d)", sda_pin, scl_pin);
    return ESP_OK;
}

static bool ina219_present(void) {
    if (!s_i2c_bus) return false;
    return i2c_master_probe(s_i2c_bus, INA219_I2C_ADDR, INA219_PROBE_TIMEOUT_MS) == ESP_OK;
}

static esp_err_t ina219_attach(void) {
    if (s_ina219_dev) return ESP_OK;  // idempotent
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = INA219_I2C_ADDR,
        .scl_speed_hz    = INA219_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_ina219_dev);
}

static esp_err_t ina219_read_reg(uint8_t reg, uint16_t *out) {
    if (!s_ina219_dev) return ESP_ERR_INVALID_STATE;
    uint8_t cmd = reg;
    uint8_t buf[2] = {0};
    esp_err_t err = i2c_master_transmit_receive(
        s_ina219_dev, &cmd, 1, buf, 2, INA219_READ_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    *out = ((uint16_t)buf[0] << 8) | buf[1];
    return ESP_OK;
}

static esp_err_t ina219_write_reg(uint8_t reg, uint16_t val) {
    if (!s_ina219_dev) return ESP_ERR_INVALID_STATE;
    uint8_t buf[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    return i2c_master_transmit(s_ina219_dev, buf, 3, INA219_READ_TIMEOUT_MS);
}

// Configure INA219 for our workload: 16V bus range, ÷4 shunt gain
// (1.6 A max), 16-sample averaging on both ADCs, continuous mode.
// Must be called AFTER ina219_attach(). After write, allow ~17 ms before
// first read so the first averaged sample is ready.
// INA219 power-down config word: MODE field (bits 0-2) = 000.
// Other bits retain BRNG=0 / PG=10 / BADC/SADC settings so when the chip
// wakes (next config write in ina219_configure) it returns to the same
// measurement regime without an extra round-trip.
#define INA219_CONFIG_POWERDOWN  ((uint16_t)((INA219_CONFIG_VALUE) & 0xFFF8))

static esp_err_t ina219_configure(void) {
    esp_err_t err = ina219_write_reg(INA219_REG_CONFIG, INA219_CONFIG_VALUE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "INA219 config write failed: %s", esp_err_to_name(err));
        return err;
    }
    // Wait one averaged-conversion period (16 samples × ~1ms ≈ 17ms total
    // across both ADCs) so the first read returns settled values, not the
    // partially-converted state captured at config-write time.
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "INA219 configured: 16V/×4/16-avg/continuous (cfg=0x%04X)",
             INA219_CONFIG_VALUE);
    return ESP_OK;
}

static esp_err_t ina219_read(power_reading_t *out) {
    uint16_t shunt_raw_u, bus_raw_u;
    esp_err_t err = ina219_read_reg(INA219_REG_SHUNT_V, &shunt_raw_u);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "INA219 shunt read failed: %s", esp_err_to_name(err));
        return err;
    }
    err = ina219_read_reg(INA219_REG_BUS_V, &bus_raw_u);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "INA219 bus read failed: %s", esp_err_to_name(err));
        return err;
    }

    // Shunt voltage: signed 16-bit, LSB 10 µV; current_mA = raw / 10 (signed).
    int16_t shunt_raw = (int16_t)shunt_raw_u;
    int32_t current_ma = (int32_t)shunt_raw / INA219_SHUNT_DIV_PER_MA;

    // Bus voltage: top 13 bits, LSB 4 mV; bottom 3 bits are CNVR/OVF flags.
    uint32_t vbus_mv = (uint32_t)((bus_raw_u >> 3) & 0x1FFFu) * 4u;

    int32_t power_mw = (int32_t)(((int64_t)vbus_mv * current_ma) / 1000);

    out->mode       = POWER_MODE_INA219;
    out->vbat_mv    = vbus_mv;
    out->pct        = mv_to_pct(vbus_mv);
    out->current_ma = current_ma;
    out->power_mw   = power_mw;
    out->charging   = (current_ma < 0);  // current INTO battery → charging

    ESP_LOGI(TAG, "INA219: %lumV  %ldmA  %ldmW  %s  → %d%%",
             (unsigned long)vbus_mv, (long)current_ma, (long)power_mw,
             out->charging ? "CHARGING" : "discharging", out->pct);
    return ESP_OK;
}

// ── NVS override ─────────────────────────────────────────────────────────────
esp_err_t power_get_override(char *out, size_t out_len) {
    if (!out || out_len == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    if (nvs_open(NVS_NS_PWR, NVS_READONLY, &h) != ESP_OK) {
        strncpy(out, DEFAULT_OVERRIDE, out_len);
        out[out_len - 1] = '\0';
        return ESP_OK;
    }

    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, NVS_KEY_PWR_OVERRIDE, out, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        strncpy(out, DEFAULT_OVERRIDE, out_len);
        out[out_len - 1] = '\0';
    }
    return ESP_OK;
}

esp_err_t power_set_override(const char *mode_str) {
    if (!mode_str) return ESP_ERR_INVALID_ARG;
    // "voltage" rejected — mode removed 2026-05-16.
    if (strcmp(mode_str, "auto")     != 0 &&
        strcmp(mode_str, "ina219")   != 0 &&
        strcmp(mode_str, "disabled") != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_PWR, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, NVS_KEY_PWR_OVERRIDE, mode_str);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "Power override set to '%s' (effective on next boot)", mode_str);
    return err;
}

// ── Public API ───────────────────────────────────────────────────────────────
esp_err_t power_init(int sda_pin, int scl_pin) {
    char override[MAX_OVERRIDE_LEN] = {0};
    power_get_override(override, sizeof(override));
    ESP_LOGI(TAG, "power_init: override='%s'", override);

    if (strcmp(override, "disabled") == 0) {
        s_mode = POWER_MODE_NONE;
        ESP_LOGI(TAG, "Power monitoring DISABLED (override)");
        return ESP_OK;
    }

    // "auto" or "ina219" → init I²C + probe.
    esp_err_t err = i2c_bus_init(sda_pin, scl_pin);
    if (err != ESP_OK) {
        s_mode = POWER_MODE_NONE;
        ESP_LOGW(TAG, "I²C bus init failed — power monitoring disabled");
        return ESP_OK;  // non-fatal; TX still functions, just no power telemetry
    }

    if (!ina219_present()) {
        s_mode = POWER_MODE_NONE;
        ESP_LOGW(TAG, "INA219 not found at 0x%02X — power monitoring disabled",
                 INA219_I2C_ADDR);
        return ESP_OK;
    }

    err = ina219_attach();
    if (err != ESP_OK) {
        s_mode = POWER_MODE_NONE;
        ESP_LOGW(TAG, "INA219 attach failed: %s — power monitoring disabled",
                 esp_err_to_name(err));
        return ESP_OK;
    }

    // Apply our preferred config (16V/×4 gain/16-sample averaging/continuous).
    // Non-fatal if it fails — chip will still work with POR defaults, just
    // noisier readings.
    ina219_configure();

    s_mode = POWER_MODE_INA219;
    ESP_LOGI(TAG, "INA219 power monitor ready at 0x%02X", INA219_I2C_ADDR);
    return ESP_OK;
}

esp_err_t power_read(power_reading_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->mode = s_mode;

    if (s_mode == POWER_MODE_NONE) {
        return ESP_OK;  // all-zero reading, mode reflects NONE
    }

    esp_err_t err = ina219_read(out);
    if (err != ESP_OK) {
        // Transient I²C error — return zero-reading struct rather than failing
        // hard. Caller decides how to surface this (typically: keep last good
        // value, or just skip this wake cycle).
        out->vbat_mv = 0;
        out->pct = 0;
        out->current_ma = 0;
        out->power_mw = 0;
        out->charging = false;
        return err;
    }
    return ESP_OK;
}

power_mode_t power_get_mode(void) {
    return s_mode;
}

const char *power_mode_str(power_mode_t mode) {
    switch (mode) {
        case POWER_MODE_NONE:    return "none";
        case POWER_MODE_INA219:  return "ina219";
    }
    return "unknown";
}

char power_mode_char(power_mode_t mode) {
    switch (mode) {
        case POWER_MODE_NONE:    return 'n';
        case POWER_MODE_INA219:  return 'i';
    }
    return '?';
}

power_mode_t power_mode_from_char(char c) {
    switch (c) {
        case 'i': case 'I': return POWER_MODE_INA219;
        case 'v': case 'V': // legacy voltage mode no longer exists → NONE
        case 'n': case 'N':
        default:            return POWER_MODE_NONE;
    }
}

void power_sleep(void) {
    if (s_mode != POWER_MODE_INA219) return;
    // Write MODE=000 (power-down). Other config bits (BRNG/PG/BADC/SADC)
    // are kept identical to INA219_CONFIG_VALUE so they're not the source
    // of drift on re-wake. Register contents are retained in power-down per
    // the datasheet, so power_init's ina219_configure() on next wake will
    // re-enable the same regime cleanly. Don't bother waiting for the write
    // to complete — we're about to deep-sleep anyway.
    esp_err_t err = ina219_write_reg(INA219_REG_CONFIG, INA219_CONFIG_POWERDOWN);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "INA219 sleep write failed: %s (continuing to deep sleep)",
                 esp_err_to_name(err));
    }
}
