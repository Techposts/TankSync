/**
 * sensor_ld2413 — HLK-LD2413 24GHz mmWave liquid-level radar driver.
 *
 * Protocol (per HLK-LD2413 User Manual V1.0, 2024-08-23):
 *   - UART: 115200 baud, 8N1
 *   - Supply: 3.3V (3.0-3.6V)
 *   - Range: 0.15-10.5 m, accuracy +/-3mm
 *   - Default reporting cycle: 160ms (configurable 50-1000ms)
 *
 * Report frame (sensor → host, 14 bytes, little-endian):
 *   F4 F3 F2 F1 | <2-byte length LE> | <4-byte float32 LE distance mm> | F8 F7 F6 F5
 *
 * Command frame (host → sensor, only used for config — not needed for
 * factory-stock operation):
 *   FD FC FB FA | <2-byte length LE> | <2-byte cmd LE> | <value> | 04 03 02 01
 *
 * STATUS: built from datasheet, NOT bench-verified on hardware. Mark
 * untested in release notes per feedback_test_before_release. Selecting
 * "ld2413" in the TX UI today rides this driver — verify with a bench
 * unit before promoting LD2413 to a shipping SKU.
 *
 * Filtering: same IQR-median pipeline used by sensor_sr04. 5 samples per
 * call (~800ms worth of frames at default 160ms cycle), drop outliers,
 * return median in cm.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/uart.h"

#ifndef LD2413_MIN_CM
#define LD2413_MIN_CM           15      // 0.15m sensor floor
#endif
#ifndef LD2413_MAX_CM
#define LD2413_MAX_CM           1050    // 10.5m sensor ceiling
#endif
#ifndef LD2413_SAMPLES
#define LD2413_SAMPLES          5       // Frames per call, IQR-filtered median
#endif
#ifndef LD2413_READ_TIMEOUT_MS
#define LD2413_READ_TIMEOUT_MS  3000    // Hard wall — bail if we can't get enough samples
#endif

/**
 * Initialize the UART peripheral for LD2413 frame streaming.
 * Caller must have powered the sensor (PIN_5V_GATE high) and waited the
 * warmup window. Subsequent ld2413_read_cm() calls drain the UART buffer
 * and read fresh frames.
 *
 * @param uart_num  UART peripheral to use (e.g. UART_NUM_0; LoRa uses UART_NUM_1)
 * @param pin_tx    ESP32 GPIO to drive sensor's RX pin
 * @param pin_rx    ESP32 GPIO connected to sensor's OT1 (UART_TX) pin
 */
esp_err_t ld2413_init(uart_port_t uart_num, int pin_tx, int pin_rx);

/**
 * Take a robust distance reading.
 *
 * Reads LD2413_SAMPLES valid frames (with hard timeout LD2413_READ_TIMEOUT_MS),
 * applies IQR-median filtering, returns result in cm.
 *
 * @param out_cm  Output distance in cm (rounded from sensor's mm float)
 * @return ESP_OK on success
 *         ESP_ERR_INVALID_RESPONSE if fewer than 3 valid frames within timeout
 *         ESP_ERR_TIMEOUT if no header bytes seen at all
 *         ESP_ERR_INVALID_STATE if init was not called
 */
esp_err_t ld2413_read_cm(int *out_cm);

/**
 * Release the UART driver. Safe to call before deep_sleep.
 */
void ld2413_deinit(void);
