// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

/**
 * battery_monitor / power_monitor — battery & power telemetry for TX
 *
 * Two hardware variants supported:
 *
 *   Variant A — voltage divider on ADC (legacy battery_init / battery_read)
 *     Vbat → R1(100k) → ADC → R2(100k) → GND
 *     ADC reads Vbat/2; non-linear LiPo 3-point curve maps mV → percentage.
 *
 *   Variant B — INA219 over I²C (single shunt in battery+ lead, bidirectional)
 *     Bus voltage register → battery voltage (no divider needed)
 *     Shunt voltage register → signed current (positive = discharging)
 *     Power = V_bus × I_shunt computed in software
 *
 * Auto-detect at boot probes I²C address 0x40. NVS-stored override
 * (key "pwr_mode_ovr") can force a specific mode: "auto" / "voltage" /
 * "ina219" / "disabled". Default: "auto".
 *
 * The legacy battery_init / battery_read API is preserved as thin wrappers
 * so existing callers compile unchanged.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// ── Battery constants (override before include if needed) ────────────────────
#ifndef BAT_DIVIDER_RATIO
#define BAT_DIVIDER_RATIO       2       // Voltage divider (Vbat split equally)
#endif
#ifndef BAT_ADC_CORRECTION
#define BAT_ADC_CORRECTION      1.0f    // Trim factor for ADC offset
#endif

// ── Power-monitor public types ───────────────────────────────────────────────

typedef enum {
    POWER_MODE_NONE     = 0,   // disabled (no readings)
    POWER_MODE_VOLTAGE  = 1,   // ADC voltage-divider only
    POWER_MODE_INA219   = 2,   // INA219 over I²C (current + voltage + power)
} power_mode_t;

typedef struct {
    power_mode_t mode;
    int      pct;          // 0–100 estimated state-of-charge
    uint32_t vbat_mv;      // battery voltage in mV
    int32_t  current_ma;   // signed; +ve = discharging, -ve = charging
                           // 0 in POWER_MODE_VOLTAGE / NONE
    int32_t  power_mw;     // V_bus × I; signed-magnitude follows current
                           // 0 in POWER_MODE_VOLTAGE / NONE
    bool     charging;     // explicit flag (current sign in INA219 mode,
                           // voltage trend / unknown in voltage mode)
} power_reading_t;

// ── Power-monitor public API ─────────────────────────────────────────────────

/**
 * Initialize power monitoring with auto-detect + NVS override support.
 *
 * Boot sequence:
 *   1. Read NVS "pwr_mode_ovr" (default "auto" if absent)
 *   2. If "disabled" → mode = NONE, return ESP_OK
 *   3. If "voltage" → init ADC, mode = VOLTAGE
 *   4. If "ina219"  → init I²C, probe 0x40 → mode = INA219 if found
 *                                          → fall back to ADC if NACK
 *   5. If "auto"    → init I²C, probe 0x40 → INA219 if found, ADC otherwise
 *
 * @param adc_channel  ADC1 channel for voltage divider (e.g. ADC_CHANNEL_0)
 * @param sda_pin      GPIO for I²C SDA (used in INA219 / auto modes; -1 to skip)
 * @param scl_pin      GPIO for I²C SCL (used in INA219 / auto modes; -1 to skip)
 */
esp_err_t power_init(int adc_channel, int sda_pin, int scl_pin);

/**
 * Read latest power telemetry. Populates all fields per the active mode.
 * Fields not measured by the active mode are set to 0.
 */
esp_err_t power_read(power_reading_t *out);

/** Currently active power mode (after auto-detect / override resolution). */
power_mode_t power_get_mode(void);

/** Lower-case string form of a mode: "none" / "voltage" / "ina219". */
const char *power_mode_str(power_mode_t mode);

/**
 * Single-character mode tag for the LoRa packet:
 *   'n' = none, 'v' = voltage, 'i' = ina219
 * Use this in the on-air format for a 1-byte mode field.
 */
char power_mode_char(power_mode_t mode);

/** Parse a 1-char mode tag from the LoRa packet back into the enum. */
power_mode_t power_mode_from_char(char c);

/**
 * Set the persisted NVS override. Accepts:
 *   "auto"     → re-detect on next boot
 *   "voltage"  → force ADC voltage-divider
 *   "ina219"   → force INA219 (boot fails over to ADC if not present)
 *   "disabled" → no power monitoring
 * Caller must reboot for the override to take effect.
 *
 * @return ESP_OK if accepted, ESP_ERR_INVALID_ARG for unknown strings.
 */
esp_err_t power_set_override(const char *mode_str);

/** Read the current persisted override into a caller-supplied buffer. */
esp_err_t power_get_override(char *out, size_t out_len);

// ── Legacy API (preserved for backward compatibility) ────────────────────────

/**
 * Initialize ADC for battery voltage measurement (legacy).
 * Equivalent to power_init(adc_channel, -1, -1) — no I²C, no auto-detect.
 *
 * @param adc_channel  ADC channel connected to voltage divider midpoint
 */
esp_err_t battery_init(int adc_channel);

/**
 * Read battery level (legacy thin wrapper over power_read).
 * @param out_pct  Output: 0-100%
 * @param out_mv   Output: battery voltage in mV (post-divider correction)
 */
esp_err_t battery_read(int *out_pct, uint32_t *out_mv);
