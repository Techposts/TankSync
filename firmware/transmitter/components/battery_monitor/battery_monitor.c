// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

/**
 * battery_monitor implementation
 *
 * ESP32-C3 ADC1: 12-bit, Vref ≈ 3.3V
 * Voltage divider: Vbat → R1(100k) → ADC → R2(100k) → GND
 * → ADC reads Vbat/2, so Vbat = adc_mv * 2
 *
 * Non-linear LiPo discharge curve (3 breakpoints + linear interpolation):
 *   4200mV → 100%
 *   3700mV → 50%
 *   3000mV → 0%
 */

#include "battery_monitor.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include <string.h>

static const char *TAG = "battery";

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t         s_cali       = NULL;
static int                       s_channel    = ADC_CHANNEL_0;

// ── Non-linear LiPo discharge curve ──────────────────────────────────────────
// Returns percentage for a given voltage in mV
static int mv_to_pct(uint32_t mv) {
    if (mv >= 4200) return 100;
    if (mv <= 3000) return 0;

    // Upper segment: 3700-4200mV → 50-100%
    if (mv >= 3700) {
        return 50 + (int)((mv - 3700) * 50 / (4200 - 3700));
    }

    // Lower segment: 3000-3700mV → 0-50%
    return (int)((mv - 3000) * 50 / (3700 - 3000));
}

esp_err_t battery_init(int adc_channel) {
    s_channel = adc_channel;

    // Configure ADC unit 1
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Configure channel
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,    // 0-3.3V range
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc_handle, s_channel, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    // Try to init calibration (curve fitting if available, else line fitting)
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .chan     = s_channel,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        s_cali = NULL;
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        s_cali = NULL;
    }
#endif
    ESP_LOGI(TAG, "ADC init OK, calibration: %s", s_cali ? "enabled" : "raw");
    return ESP_OK;
}

esp_err_t battery_read(int *out_pct, uint32_t *out_mv) {
    if (!s_adc_handle) return ESP_ERR_INVALID_STATE;

    // Average 8 reads to reduce noise
    int64_t raw_sum = 0;
    for (int i = 0; i < 8; i++) {
        int raw;
        adc_oneshot_read(s_adc_handle, s_channel, &raw);
        raw_sum += raw;
    }
    int raw_avg = (int)(raw_sum / 8);

    // Convert to mV
    uint32_t adc_mv = 0;
    if (s_cali) {
        int mv;
        adc_cali_raw_to_voltage(s_cali, raw_avg, &mv);
        adc_mv = (uint32_t)mv;
    } else {
        // Rough conversion: 3300mV / 4095 counts
        adc_mv = (uint32_t)((int64_t)raw_avg * 3300 / 4095);
    }

    // Apply voltage divider ratio and correction factor
    uint32_t vbat_mv = (uint32_t)(adc_mv * BAT_DIVIDER_RATIO * BAT_ADC_CORRECTION);

    *out_mv  = vbat_mv;
    *out_pct = mv_to_pct(vbat_mv);

    ESP_LOGI(TAG, "ADC raw=%d → %lumV (Vbat=%lumV) → %d%%",
             raw_avg, (unsigned long)adc_mv,
             (unsigned long)vbat_mv, *out_pct);
    return ESP_OK;
}
