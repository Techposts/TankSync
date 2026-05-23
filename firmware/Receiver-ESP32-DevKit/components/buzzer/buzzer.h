// Active 3-pin buzzer driver — 14 alert events, Silver profiles (Quiet/Standard/Loud),
// quiet-hours window, master mute. Caller is responsible for re-trigger cadence —
// buzzer_play() is fire-once-and-return. See project_buzzer_design_2026_05_23.md.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Alert events. Order is persisted in NVS — never reorder or remove entries;
// only append. Update BUZZ_EVENT_NAMES[] in buzzer.c to match.
typedef enum {
    // Tier 1 — default-on, always available
    BUZZ_BOOT_COMPLETE = 0,
    BUZZ_CRITICAL_LOW,        // tank <5%
    BUZZ_OVERFLOW,            // tank >95%
    BUZZ_SENSOR_OFFLINE,      // TX silent >15 min
    BUZZ_TEST_BUTTON,         // "Play test sound" UI button
    // Tier 2 — default-on, user can disable
    BUZZ_LOW_WARNING,         // tank <20%
    BUZZ_PAIR_SUCCESS,        // hub claimed or TX paired
    BUZZ_OTA_SUCCESS,
    BUZZ_OTA_FAILURE,
    // Tier 3 — default-off, opt-in
    BUZZ_REFILL_DETECTED,
    BUZZ_REFILL_COMPLETE,
    BUZZ_DRAIN_DETECTED,
    BUZZ_WIFI_RECONNECTED,
    BUZZ_TX_LOW_BATTERY,
    BUZZ__COUNT,
} buzzer_event_t;

typedef enum {
    BUZZ_PROFILE_QUIET    = 0,
    BUZZ_PROFILE_STANDARD = 1,
    BUZZ_PROFILE_LOUD     = 2,
} buzzer_profile_t;

typedef struct {
    bool        master_enable;                  // master mute toggle (boot tone bypasses)
    bool        alert_enable[BUZZ__COUNT];      // per-alert on/off
    uint8_t     master_profile;                 // buzzer_profile_t — affects ALL alerts uniformly
    uint8_t     quiet_start_hour;               // 0-23 local time; quiet window applied to non-critical alerts
    uint8_t     quiet_end_hour;
    bool        critical_overrides_quiet;       // CRITICAL_LOW plays during quiet hours regardless
} buzzer_config_t;

// Bring up GPIO + spawn buzzer_task. Loads NVS config (defaults applied if missing).
// `gpio_num` is the board-specific buzzer pin (PIN_BUZZER from the active pinmap).
esp_err_t buzzer_init(int gpio_num);

// Queue an event for playback. Honors master_enable, alert_enable[], quiet hours.
// Caller is responsible for repeat cadence (e.g. call every 60s while critical-low persists).
void buzzer_play(buzzer_event_t evt);

// Bypass enable + quiet-hours checks. Used by the "Play test sound" UI button.
// `profile_override` lets the UI preview a non-current profile without saving.
void buzzer_test(buzzer_event_t evt, buzzer_profile_t profile_override);

// Read current config (copy into caller-owned struct).
void buzzer_get_config(buzzer_config_t *out);

// Persist new config to NVS and apply atomically.
esp_err_t buzzer_set_config(const buzzer_config_t *in);

// Stable string for UI labels (e.g. "Critical low (<5%)"). Returned pointer is static.
const char *buzzer_event_label(buzzer_event_t evt);

// Default config (Tier 1+2 enabled, Tier 3 disabled, Standard profile, quiet 22-07, critical overrides).
void buzzer_default_config(buzzer_config_t *out);
