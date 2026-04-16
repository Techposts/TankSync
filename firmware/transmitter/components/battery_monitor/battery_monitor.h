// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

/**
 * battery_monitor - LiPo battery level via ADC
 *
 * Uses a non-linear 3-point discharge curve for LiPo:
 *   3.0V → 0%
 *   3.7V → 50%
 *   4.2V → 100%
 *
 * Averages 8 ADC reads to reduce noise.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

// Battery constants (override before include if needed)
#ifndef BAT_DIVIDER_RATIO
#define BAT_DIVIDER_RATIO       2       // Voltage divider (Vbat split equally)
#endif
#ifndef BAT_ADC_CORRECTION
#define BAT_ADC_CORRECTION      1.0f    // Trim factor for ADC offset
#endif

/**
 * Initialize ADC for battery voltage measurement.
 * @param adc_channel  ADC channel connected to voltage divider midpoint
 */
esp_err_t battery_init(int adc_channel);

/**
 * Read battery level.
 * @param out_pct  Output: 0-100%
 * @param out_mv   Output: battery voltage in mV (before divider correction)
 */
esp_err_t battery_read(int *out_pct, uint32_t *out_mv);
