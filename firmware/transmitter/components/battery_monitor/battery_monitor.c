// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

/**
 * battery_monitor / power_monitor implementation
 *
 * Two operating modes (selected by NVS override or auto-detect at init):
 *
 *   POWER_MODE_VOLTAGE — ESP32-C3 ADC1 + voltage divider
 *     12-bit ADC, Vref ≈ 3.3V, divider Vbat → R1(100k) → ADC → R2(100k) → GND
 *     ADC reads Vbat/2; non-linear LiPo 3-point curve maps mV → percentage.
 *
 *   POWER_MODE_INA219 — I²C power monitor at 0x40
 *     Bus voltage register (0x02) → battery voltage (LSB 4 mV after >>3)
 *     Shunt voltage register (0x01) → signed 10 µV LSB across 0.1 Ω shunt
 *     → current_mA = shunt_raw / 10 (positive = discharging, negative = charging)
 *     Power computed in software as V × I to avoid INA219 calibration register.
 *
 * Auto-detect probes 0x40 over the configured I²C bus. NVS override at
 * key "pwr_mode_ovr" in namespace "system" forces a mode if set.
 */

#include "battery_monitor.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/i2c_master.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "power";

// ── Tuning constants (overridable via -D in CMakeLists or config.h) ──────────
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
// 0.1 Ω shunt = 100 mΩ. With 10 µV INA219 shunt LSB:
//   I (mA) = (raw × 10 µV) / 0.1 Ω = raw × 0.1 mA
//   → shunt_raw / 10 = current in mA (signed)
#ifndef INA219_SHUNT_DIV_PER_MA
#define INA219_SHUNT_DIV_PER_MA     10
#endif

#define NVS_NS_PWR              "system"
#define NVS_KEY_PWR_OVERRIDE    "pwr_mode_ovr"
#define DEFAULT_OVERRIDE        "auto"
#define MAX_OVERRIDE_LEN        12

// INA219 registers
#define INA219_REG_CONFIG       0x00
#define INA219_REG_SHUNT_V      0x01
#define INA219_REG_BUS_V        0x02

// ── Module state ─────────────────────────────────────────────────────────────
static power_mode_t              s_mode             = POWER_MODE_NONE;
static int                       s_adc_channel      = ADC_CHANNEL_0;
static adc_oneshot_unit_handle_t s_adc_handle       = NULL;
static adc_cali_handle_t         s_adc_cali         = NULL;
static i2c_master_bus_handle_t   s_i2c_bus          = NULL;
static i2c_master_dev_handle_t   s_ina219_dev       = NULL;
static uint32_t                  s_last_vbat_mv     = 0;  // for charging-trend in voltage mode

// ── Non-linear LiPo discharge curve ──────────────────────────────────────────
// 3 breakpoints + linear interpolation: 3000 mV → 0%, 3700 mV → 50%, 4200 mV → 100%
static int mv_to_pct(uint32_t mv) {
    if (mv >= 4200) return 100;
    if (mv <= 3000) return 0;
    if (mv >= 3700) return 50 + (int)((mv - 3700) * 50 / (4200 - 3700));
    return (int)((mv - 3000) * 50 / (3700 - 3000));
}

// ── ADC voltage-divider path ─────────────────────────────────────────────────
static esp_err_t adc_init_internal(int channel) {
    s_adc_channel = channel;

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc_handle, channel, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(err));
        return err;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .chan     = channel,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) != ESP_OK) {
        s_adc_cali = NULL;
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_adc_cali) != ESP_OK) {
        s_adc_cali = NULL;
    }
#endif
    ESP_LOGI(TAG, "ADC ready (channel=%d, calibration=%s)",
             channel, s_adc_cali ? "enabled" : "raw");
    return ESP_OK;
}

static esp_err_t adc_read_internal(power_reading_t *out) {
    if (!s_adc_handle) return ESP_ERR_INVALID_STATE;

    int64_t raw_sum = 0;
    for (int i = 0; i < 8; i++) {
        int raw;
        adc_oneshot_read(s_adc_handle, s_adc_channel, &raw);
        raw_sum += raw;
    }
    int raw_avg = (int)(raw_sum / 8);

    uint32_t adc_mv = 0;
    if (s_adc_cali) {
        int mv;
        adc_cali_raw_to_voltage(s_adc_cali, raw_avg, &mv);
        adc_mv = (uint32_t)mv;
    } else {
        adc_mv = (uint32_t)((int64_t)raw_avg * 3300 / 4095);
    }

    uint32_t vbat_mv = (uint32_t)(adc_mv * BAT_DIVIDER_RATIO * BAT_ADC_CORRECTION);

    out->mode       = POWER_MODE_VOLTAGE;
    out->vbat_mv    = vbat_mv;
    out->pct        = mv_to_pct(vbat_mv);
    out->current_ma = 0;
    out->power_mw   = 0;
    // Charging inference: voltage rising vs last reading.
    // First read after boot has s_last_vbat_mv == 0 → report charging=false until baseline exists.
    if (s_last_vbat_mv == 0) {
        out->charging = false;
    } else {
        out->charging = (vbat_mv > s_last_vbat_mv + 20);  // 20 mV hysteresis vs noise
    }
    s_last_vbat_mv = vbat_mv;

    ESP_LOGI(TAG, "ADC raw=%d → %lumV (Vbat=%lumV) → %d%%",
             raw_avg, (unsigned long)adc_mv, (unsigned long)vbat_mv, out->pct);
    return ESP_OK;
}

// ── I²C bus + INA219 path ────────────────────────────────────────────────────
static esp_err_t i2c_bus_init_internal(int sda_pin, int scl_pin) {
    if (s_i2c_bus) return ESP_OK;  // idempotent
    if (sda_pin < 0 || scl_pin < 0) return ESP_ERR_INVALID_ARG;

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1,                      // auto-allocate
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

static esp_err_t ina219_attach_device(void) {
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

static esp_err_t ina219_read_internal(power_reading_t *out) {
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

    // Shunt voltage register is signed 16-bit, LSB 10 µV.
    // Across our 0.1 Ω shunt, current_mA = shunt_raw / 10 (signed).
    int16_t shunt_raw = (int16_t)shunt_raw_u;
    int32_t current_ma = (int32_t)shunt_raw / INA219_SHUNT_DIV_PER_MA;

    // Bus voltage register: top 13 bits hold voltage in 4 mV LSB.
    // Bit 1 = CNVR (conversion ready), bit 0 = OVF (math overflow).
    uint32_t vbus_mv = (uint32_t)((bus_raw_u >> 3) & 0x1FFFu) * 4u;

    int32_t power_mw = (int32_t)(((int64_t)vbus_mv * current_ma) / 1000);

    out->mode       = POWER_MODE_INA219;
    out->vbat_mv    = vbus_mv;
    out->pct        = mv_to_pct(vbus_mv);
    out->current_ma = current_ma;
    out->power_mw   = power_mw;
    out->charging   = (current_ma < 0);

    ESP_LOGI(TAG, "INA219: %lumV  %ldmA  %ldmW  %s  → %d%%",
             (unsigned long)vbus_mv, (long)current_ma, (long)power_mw,
             out->charging ? "CHARGING" : "discharging", out->pct);
    return ESP_OK;
}

// ── NVS override read/write ──────────────────────────────────────────────────
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
    if (strcmp(mode_str, "auto")     != 0 &&
        strcmp(mode_str, "voltage")  != 0 &&
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

// ── Auto-detect resolution ───────────────────────────────────────────────────
static power_mode_t resolve_mode(const char *override, int sda, int scl) {
    if (strcmp(override, "disabled") == 0) {
        return POWER_MODE_NONE;
    }

    if (strcmp(override, "voltage") == 0) {
        return POWER_MODE_VOLTAGE;
    }

    bool i2c_ok = (sda >= 0 && scl >= 0) && (i2c_bus_init_internal(sda, scl) == ESP_OK);

    if (strcmp(override, "ina219") == 0) {
        if (i2c_ok && ina219_present()) return POWER_MODE_INA219;
        ESP_LOGW(TAG, "Forced INA219 but device not found; falling back to voltage");
        return POWER_MODE_VOLTAGE;
    }

    // override == "auto" (or anything unknown — treat as auto)
    if (i2c_ok && ina219_present()) {
        ESP_LOGI(TAG, "Auto-detect: INA219 found at 0x40");
        return POWER_MODE_INA219;
    }
    ESP_LOGI(TAG, "Auto-detect: no INA219 found, using voltage divider");
    return POWER_MODE_VOLTAGE;
}

// ── Public API implementations ───────────────────────────────────────────────
esp_err_t power_init(int adc_channel, int sda_pin, int scl_pin) {
    char override[MAX_OVERRIDE_LEN] = {0};
    power_get_override(override, sizeof(override));
    ESP_LOGI(TAG, "power_init: override='%s'", override);

    s_mode = resolve_mode(override, sda_pin, scl_pin);

    switch (s_mode) {
        case POWER_MODE_NONE:
            ESP_LOGI(TAG, "Power monitoring DISABLED");
            return ESP_OK;

        case POWER_MODE_VOLTAGE:
            return adc_init_internal(adc_channel);

        case POWER_MODE_INA219:
            // I²C bus already up via resolve_mode → ina219_present().
            // Attach the device handle for read operations.
            return ina219_attach_device();
    }
    return ESP_FAIL;
}

esp_err_t power_read(power_reading_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->mode = s_mode;

    switch (s_mode) {
        case POWER_MODE_NONE:
            return ESP_OK;  // all-zero reading
        case POWER_MODE_VOLTAGE:
            return adc_read_internal(out);
        case POWER_MODE_INA219: {
            esp_err_t err = ina219_read_internal(out);
            if (err != ESP_OK) {
                // Transient I²C error — return zero-reading struct rather than
                // failing hard. Caller can decide how to surface this.
                out->vbat_mv = 0;
                out->pct = 0;
                return err;
            }
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

power_mode_t power_get_mode(void) {
    return s_mode;
}

const char *power_mode_str(power_mode_t mode) {
    switch (mode) {
        case POWER_MODE_NONE:    return "none";
        case POWER_MODE_VOLTAGE: return "voltage";
        case POWER_MODE_INA219:  return "ina219";
    }
    return "unknown";
}

char power_mode_char(power_mode_t mode) {
    switch (mode) {
        case POWER_MODE_NONE:    return 'n';
        case POWER_MODE_VOLTAGE: return 'v';
        case POWER_MODE_INA219:  return 'i';
    }
    return '?';
}

power_mode_t power_mode_from_char(char c) {
    switch (c) {
        case 'n': case 'N': return POWER_MODE_NONE;
        case 'v': case 'V': return POWER_MODE_VOLTAGE;
        case 'i': case 'I': return POWER_MODE_INA219;
    }
    return POWER_MODE_NONE;
}

// ── Legacy wrappers ──────────────────────────────────────────────────────────
esp_err_t battery_init(int adc_channel) {
    // Legacy path: voltage divider only, no I²C, no auto-detect.
    s_mode = POWER_MODE_VOLTAGE;
    return adc_init_internal(adc_channel);
}

esp_err_t battery_read(int *out_pct, uint32_t *out_mv) {
    power_reading_t pr;
    esp_err_t err = power_read(&pr);
    if (err != ESP_OK) return err;
    if (out_pct) *out_pct = pr.pct;
    if (out_mv)  *out_mv  = pr.vbat_mv;
    return ESP_OK;
}
