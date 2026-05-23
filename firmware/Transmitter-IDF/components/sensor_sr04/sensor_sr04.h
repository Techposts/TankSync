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

// Sensor timing and filtering constants.
// NOTE: main/config.h cannot override these — sensor_sr04.c doesn't include
// config.h, so the #ifndef block always falls through to the default in this
// translation unit. Edit the value HERE if you need to change it.
#ifndef SENSOR_TRIG_US
#define SENSOR_TRIG_US          20      // Trigger pulse width (µs) — JSN-SR04T datasheet requires ≥10µs; 20µs is the bench-validated reliable minimum.
#endif
#ifndef SENSOR_TIMEOUT_US
#define SENSOR_TIMEOUT_US       30000   // Echo pulse-width timeout ≈ 5m (30000µs)
#endif
// JSN-SR04M / AJ-SR04M variants take 10–15ms after TRIG before they assert
// ECHO (their internal MCU runs a longer pre-measurement / settling cycle).
// The original 5ms wait-for-rise was tuned for HC-SR04 / JSN-SR04T which
// respond in under 500µs — too short for the M variants, which caused
// silent timeouts and fake 100% readings on the cloud (see Bug A fix
// 2026-05-22). Bench-validated 2026-05-22 on a JSN-SR04M: echo rises at
// ~12.5ms post-trigger. 30ms gives generous headroom for slow variants
// while still catching long real distances within a wake cycle.
#ifndef SENSOR_ECHO_WAIT_US
#define SENSOR_ECHO_WAIT_US     30000
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
