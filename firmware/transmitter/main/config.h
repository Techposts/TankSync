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
#define FIRMWARE_VERSION        "2.0.3"
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
// BATTERY - ADC on internal voltage divider
// ============================================================================
// ESP32-C3 can measure its own supply via ADC channel
// Typical: VBAT → 100k → ADC pin → 100k → GND (1:2 divider)
// Raw ADC range: 0-4095 (12-bit), Vref = 3.3V
// Vbat = adc_mv * 2 (for 1:2 divider)
#define BAT_ADC_CHANNEL         0               // ADC_CHANNEL_0 = GPIO0 (check schematic)
#define BAT_DIVIDER_RATIO       2               // Voltage divider ratio
#define BAT_MIN_MV              3000            // 0% (LiPo cutoff)
#define BAT_MAX_MV              4200            // 100% (LiPo full charge)
// Correction factor for ADC non-linearity (empirical; calibrate per board)
#define BAT_ADC_CORRECTION      1.0f

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
