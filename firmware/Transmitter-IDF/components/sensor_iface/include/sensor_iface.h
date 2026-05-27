/**
 * sensor_iface — unified driver interface for TX distance sensors.
 *
 * Two drivers ship behind this vtable:
 *   - "sr04"   AJ-SR04M / JSN-SR04T ultrasonic (trig/echo bit-bang)  [available]
 *   - "ld2413" HLK-LD2413 24GHz mmWave liquid-level radar (UART)     [planned]
 *
 * main.c reads the user's sensor choice from NVS (system/sensor_kind),
 * passes it to sensor_get(), and calls iface->init() / iface->read_cm()
 * without further sensor-type awareness. Each driver knows its own pin
 * map, warmup window, and range envelope.
 *
 * Pin configuration that lives in main/config.h is passed in via the
 * driver-specific configurator (e.g. sensor_iface_sr04_set_pins) so the
 * iface component does not need to include main/config.h.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    const char *name;                          // "sr04" | "ld2413"
    esp_err_t (*init)(void);                   // power-on + peripheral setup
    esp_err_t (*read_cm)(int *out_cm);         // IQR-median filtered distance
    void      (*deinit)(void);                 // teardown (may be no-op)
    uint32_t  (*warmup_ms)(void);              // ms between power-rail-up and first usable read
    int       (*min_cm)(void);                 // sensor's published floor
    int       (*max_cm)(void);                 // sensor's published ceiling
} sensor_iface_t;

/**
 * Resolve a sensor kind string to its vtable.
 * @param kind  "sr04" | "ld2413" — case-sensitive; NULL or unknown → NULL
 * @return      vtable pointer (lifetime: static) or NULL if unrecognized
 */
const sensor_iface_t *sensor_get(const char *kind);

/**
 * Default vtable when NVS has no preference (first boot, factory reset).
 * Currently SR04 — flip this when LD2413 becomes the default SKU.
 */
const sensor_iface_t *sensor_get_default(void);

/**
 * Configure SR04 GPIO pins before iface->init() runs.
 * Must be called once at boot using PIN_TRIG / PIN_ECHO from main/config.h.
 * Safe to call repeatedly; last call wins.
 */
void sensor_iface_sr04_set_pins(int trig_pin, int echo_pin);

/**
 * Configure LD2413 UART + GPIO pins before iface->init() runs.
 * uart_num must be different from the LoRa UART (UART_NUM_1 on current
 * hardware). pin_tx / pin_rx are the ESP32-side pins — caller passes the
 * same physical pins used by SR04 (GPIO4, GPIO5) since the pin directions
 * happen to match between the two sensor types.
 *
 * @param uart_num  ESP-IDF UART peripheral number (e.g. 0 = UART_NUM_0)
 * @param pin_tx    GPIO driving the sensor's RX pin
 * @param pin_rx    GPIO receiving the sensor's OT1 (UART_TX) pin
 */
void sensor_iface_ld2413_set_uart(int uart_num, int pin_tx, int pin_rx);
