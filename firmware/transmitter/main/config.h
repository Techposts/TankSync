// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

/**
 * TankSync Transmitter v2 - Central Configuration
 * ESP32-C3 SuperMini + RYLR998 + AJ-SR04M ultrasonic
 *
 * Pin assignments match existing v1.0 hardware — same physical boards.
 */

#pragma once

// ============================================================================
// FIRMWARE
// ============================================================================
#define FIRMWARE_VERSION        "2.0.4"
#define FIRMWARE_TYPE           "transmitter"

// ============================================================================
// PINS - ESP32-C3 SuperMini (unchanged from v1.0 hardware)
// ============================================================================
#define PIN_LORA_TX             21      // UART1 TX → RYLR998 RXD
#define PIN_LORA_RX             20      // UART1 RX ← RYLR998 TXD
#define PIN_LED                 8       // On-board LED for ESP32-C3 SuperMini
#define PIN_WS2812              7       // WS2812B data (2 LEDs in series)
#define PIN_WS2812_COUNT        2       // Number of WS2812B LEDs
#define PIN_TRIG                4       // AJ-SR04M TRIG
#define PIN_ECHO                5       // AJ-SR04M ECHO
#define PIN_BUTTON              9       // Boot/config button (INPUT_PULLUP)

// ============================================================================
// SENSOR - AJ-SR04M ultrasonic
// ============================================================================
#define SENSOR_TRIG_US          10      // Trigger pulse width (µs)
#define SENSOR_TIMEOUT_US       30000   // Echo timeout = 5m (30000µs)
#define SENSOR_MIN_CM           5       // Reject readings below this
#define SENSOR_MAX_CM           400     // Reject readings above this
#define SENSOR_SAMPLES          5       // Readings per wake cycle; median used
#define SENSOR_SAMPLE_DELAY_MS  50      // Delay between samples

// Speed of sound factor: distance = echo_us / 58 (for cm)
#define SENSOR_US_TO_CM         58

// ============================================================================
// LORA - RYLR998
// ============================================================================
#define LORA_UART_NUM           UART_NUM_1
#define LORA_BAUD               115200
#define LORA_DEFAULT_FREQ       865000000   // 865 MHz
#define LORA_DEFAULT_NETID      6
#define LORA_DEFAULT_ADDR       1           // Transmitter address (unique per unit)
#define LORA_RECEIVER_ADDR      2           // Receiver's address
#define LORA_CMD_TIMEOUT_MS     1500
#define LORA_ACK_TIMEOUT_MS     3000        // Wait up to 3s for ACK
#define LORA_MAX_RETRIES        3           // Retry count if no ACK

// ============================================================================
// POWER MONITORING - Variant A (voltage divider) + Variant B (INA219 over I²C)
// ============================================================================
// Variant A wiring: VBAT → 100k → ADC → 100k → GND (1:2 divider on GPIO0).
//   ADC reads Vbat/2; firmware multiplies by BAT_DIVIDER_RATIO.
// Variant B wiring: INA219 in series with battery+, I²C on GPIO1/2 at 0x40.
//   Firmware auto-detects at boot by probing 0x40 over the I²C bus.
//   NVS key "pwr_mode_ovr" forces a mode: auto / voltage / ina219 / disabled.
//
// Both variants share the same firmware binary; only the BOM differs.

// Variant A (voltage divider)
#define BAT_ADC_CHANNEL         0               // ADC_CHANNEL_0 = GPIO0
#define BAT_DIVIDER_RATIO       2               // 1:2 divider
#define BAT_MIN_MV              3000            // 0% (LiPo cutoff)
#define BAT_MAX_MV              4200            // 100% (LiPo full charge)
#define BAT_ADC_CORRECTION      1.0f            // Trim factor (empirical)

// Variant B (INA219 over I²C)
#define PIN_I2C_SDA             1               // GPIO1 → INA219 SDA
#define PIN_I2C_SCL             2               // GPIO2 → INA219 SCL
#define INA219_I2C_ADDR         0x40            // INA219 default (A0/A1 jumpers open)
#define INA219_I2C_FREQ_HZ      100000          // 100 kHz standard mode
// 0.1 Ω shunt → shunt_raw / 10 = current in mA (signed; +ve discharge, -ve charge)

// ============================================================================
// DEEP SLEEP
// ============================================================================
#define SLEEP_INTERVAL_S        300     // 5 minutes between readings
#define SLEEP_INTERVAL_US       ((uint64_t)SLEEP_INTERVAL_S * 1000000ULL)

// ============================================================================
// NVS NAMESPACES
// ============================================================================
#define NVS_NS_LORA             "lora"
#define NVS_NS_SYSTEM           "system"

// ============================================================================
// RTC MEMORY - survives deep sleep
// ============================================================================
// msg_id counter persisted in RTC memory (no NVS write needed on each wake)
// boot_count for diagnostics
