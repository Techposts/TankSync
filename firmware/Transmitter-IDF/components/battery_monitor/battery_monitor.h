/**
 * battery_monitor / power_monitor — INA219-only (simplified 2026-05-16)
 *
 * Hardware: INA219 over I²C at 0x40, single 0.1Ω shunt in battery+ lead.
 *   Bus voltage register   → battery voltage (LSB 4 mV after >>3)
 *   Shunt voltage register → signed current (LSB 10 µV across 0.1Ω = 0.1 mA)
 *     positive shunt = current OUT of battery (discharging)
 *     negative shunt = current INTO battery (charging)
 *   Power = V_bus × I_shunt computed in software.
 *
 * Boot sequence:
 *   1. Read NVS "pwr_mode_ovr" (default "auto" if absent).
 *   2. If "disabled" → mode = NONE, no I²C bus initialised.
 *   3. If "auto" or "ina219" → init I²C, probe 0x40.
 *        present → mode = INA219.
 *        absent  → mode = NONE (no fallback). Power telemetry is
 *                  unavailable but TX otherwise runs normally.
 *
 * History: prior versions also supported a voltage-divider fallback on
 * an ADC pin (POWER_MODE_VOLTAGE). That path was removed 2026-05-16 to
 * eliminate the dual-source ambiguity and the cross-wake voltage-trend
 * charging inference, which was unreliable.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    POWER_MODE_NONE   = 0,   // disabled OR INA219 not present at boot
    POWER_MODE_INA219 = 2,   // INA219 over I²C (current + voltage + power)
    // POWER_MODE_VOLTAGE (=1) removed 2026-05-16 — kept enum value reserved
    // so legacy LoRa packets carrying 'v' parse to NONE rather than a
    // misinterpreted INA219 reading.
} power_mode_t;

typedef struct {
    power_mode_t mode;
    int      pct;          // 0–100 estimated state-of-charge (from vbat_mv)
    uint32_t vbat_mv;      // battery voltage in mV (from INA219 bus reg)
    int32_t  current_ma;   // signed; positive = discharging, negative = charging
    int32_t  power_mw;     // V_bus × I_shunt (signed; follows current sign)
    bool     charging;     // true ⇔ current_ma < 0 (current flowing INTO battery)
} power_reading_t;

/**
 * Initialize power monitoring (INA219 over I²C).
 *
 * @param sda_pin  GPIO for I²C SDA
 * @param scl_pin  GPIO for I²C SCL
 * @return ESP_OK on success — even if INA219 absent; mode just stays NONE.
 *         Errors only on truly catastrophic init failures.
 */
esp_err_t power_init(int sda_pin, int scl_pin);

/**
 * Read latest INA219 telemetry. Populates *out per the active mode.
 * In POWER_MODE_NONE returns ESP_OK with all-zero fields.
 */
esp_err_t power_read(power_reading_t *out);

/** Currently active power mode (after init + override resolution). */
power_mode_t power_get_mode(void);

/** Lower-case string form: "none" / "ina219". */
const char *power_mode_str(power_mode_t mode);

/**
 * Single-character mode tag for the LoRa packet:
 *   'n' = none, 'i' = ina219
 * 'v' (legacy voltage mode) is never sent by this firmware.
 */
char power_mode_char(power_mode_t mode);

/**
 * Parse a 1-char mode tag from the LoRa packet back into the enum.
 * 'v' (legacy) → NONE (voltage mode no longer supported).
 */
power_mode_t power_mode_from_char(char c);

/**
 * Set the persisted NVS override. Accepts:
 *   "auto"     → probe INA219 at boot, fall back to NONE if absent
 *   "ina219"   → same as "auto" in this version (kept for forward-compat
 *                with potential future modes)
 *   "disabled" → skip I²C init, no power monitoring
 * "voltage" is rejected (mode removed 2026-05-16).
 * Caller must reboot for the override to take effect.
 *
 * @return ESP_OK if accepted, ESP_ERR_INVALID_ARG for unknown strings.
 */
esp_err_t power_set_override(const char *mode_str);

/** Read the current persisted override into a caller-supplied buffer. */
esp_err_t power_get_override(char *out, size_t out_len);

/**
 * Put the INA219 into its lowest-power state (config MODE bits = 000,
 * power-down). Datasheet quiescent in this state is ~6 µA vs ~1 mA in
 * continuous mode with 16-sample averaging. Register contents are
 * retained, but power_init() will re-write the config on next wake
 * anyway, so this is fire-and-forget. No-op if mode != INA219.
 *
 * Call this from main.c right before esp_deep_sleep_start() — saves
 * ~1 mAh per hour on REV 2.2 boards where the +5V rail can't be cut
 * because the high-side P-FET stage isn't switching correctly.
 */
void power_sleep(void);
