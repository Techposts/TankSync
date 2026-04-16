// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

/**
 * sensor_sr04 - AJ-SR04M ultrasonic distance sensor driver
 *
 * Robust reading strategy:
 *   1. Take SENSOR_SAMPLES readings with a short delay between each
 *   2. Sort the readings
 *   3. Remove outliers via IQR filtering (drop top + bottom quartile)
 *   4. Return median of remaining values
 *
 * This eliminates:
 *   - False-short readings from moisture on sensor face
 *   - False-long readings from echo absorption/scattering
 *   - Occasional sensor glitches (>2x normal reading)
 *
 * Uses ESP-IDF's high-resolution timer (gptimer) for accurate µs timing.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

// Sensor timing and filtering constants (override before include if needed)
#ifndef SENSOR_TRIG_US
#define SENSOR_TRIG_US          10      // Trigger pulse width (µs)
#endif
#ifndef SENSOR_TIMEOUT_US
#define SENSOR_TIMEOUT_US       30000   // Echo timeout ≈ 5m (30000µs)
#endif
#ifndef SENSOR_US_TO_CM
#define SENSOR_US_TO_CM         58      // µs per cm (speed of sound / 2)
#endif
#ifndef SENSOR_MIN_CM
#define SENSOR_MIN_CM           5       // Reject readings below this
#endif
#ifndef SENSOR_MAX_CM
#define SENSOR_MAX_CM           400     // Reject readings above this
#endif
#ifndef SENSOR_SAMPLES
#define SENSOR_SAMPLES          5       // Readings per call; IQR-filtered median
#endif
#ifndef SENSOR_SAMPLE_DELAY_MS
#define SENSOR_SAMPLE_DELAY_MS  50      // Delay between samples
#endif

/**
 * Initialize the sensor GPIO pins.
 * @param trig_pin  GPIO for TRIG output
 * @param echo_pin  GPIO for ECHO input
 */
esp_err_t sensor_init(int trig_pin, int echo_pin);

/**
 * Take a robust distance reading.
 *
 * Internally takes SENSOR_SAMPLES readings, filters outliers,
 * and returns the median. Blocks for ~(SENSOR_SAMPLES * 100)ms.
 *
 * @param out_cm  Output distance in cm
 * @return ESP_OK if a valid reading obtained
 *         ESP_ERR_INVALID_RESPONSE if all samples invalid or filtered out
 */
esp_err_t sensor_read_cm(int *out_cm);

/**
 * Take a single raw reading (no filtering).
 * Useful for diagnostics.
 *
 * @param out_cm  Output distance in cm, or -1 on timeout
 * @return ESP_OK if echo received within timeout
 */
esp_err_t sensor_read_single_cm(int *out_cm);
