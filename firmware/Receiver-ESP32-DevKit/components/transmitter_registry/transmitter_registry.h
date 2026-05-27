/**
 * transmitter_registry - Multi-transmitter data model + persistence
 *
 * Thread-safe via mutex. Persists to SPIFFS as JSON.
 * Supports up to MAX_TRANSMITTERS tanks.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define MAX_TRANSMITTERS    10
#define TX_NAME_MAX         25      // 24 chars + null; fits "Fresh Water Tank", "Underground Sump"

// ── History buffer (Phase 2 — added v2.5.0) ──────────────────────────────────
// 48 half-hourly slots = 24 h rolling window per tank.
// Powers numeric ETAs, "yesterday's draw", and solar-charge insights without
// burdening the cloud (works offline, survives reboot, no MQTT round-trips).
#define TX_HIST_SLOTS       48
#define TX_HIST_SLOT_SECS   1800     // 30 min
#define TX_HIST_PCT_EMPTY   0xFF     // sentinel — slot has no data
#define TX_HIST_VOLT_EMPTY  0xFF     // sentinel — slot has no voltage

// ── Persistent configuration per transmitter ─────────────────────────────────
typedef struct {
    uint16_t address;               // LoRa address (1-65535, but new pairs prefer 1-99 small-int)
    uint8_t  mac[6];                // Stable hardware identity from PAIR_REQ. Zero = unknown (legacy entries paired before MAC was part of the protocol).
    char     name[TX_NAME_MAX];     // Friendly name, e.g. "Main Tank"
    int      min_dist_cm;           // Sensor reading when tank is FULL
    int      max_dist_cm;           // Sensor reading when tank is EMPTY
    float    capacity_liters;       // Total tank volume
    bool     enabled;
    // Remote config fields
    uint32_t sleep_s;               // Sleep interval in seconds
    uint8_t  samples;               // Number of sensor samples
    uint8_t  lora_pwr;              // LoRa TX power 0-22 dBm (0 = use TX default; pairing-set values otherwise)
    char     sensor_kind[12];       // "sr04" | "ld2413" — sensor driver the TX should load. Empty/unknown = leave TX at its current value.
    bool     pending_config;        // true if config needs to be sent
    // LoRa OTA fields
    bool     ota_pending;           // true if firmware update queued
    uint32_t ota_offset;            // Current byte offset in transfer
    // LED color assignment (-1 = auto from palette)
    int8_t   led_color_idx;
} tx_info_t;

// ── Connection state ─────────────────────────────────────────────────────────
typedef enum {
    TX_STATE_WAITING = 0,   // Never received data
    TX_STATE_CONNECTED,     // Fresh data (within stale threshold)
    TX_STATE_STALE,         // Data older than STALE threshold
    TX_STATE_LOST,          // Data older than LOST threshold
} tx_state_t;

// ── Runtime data per transmitter ─────────────────────────────────────────────
typedef struct {
    uint16_t  address;
    int       raw_dist_cm;
    int       water_pct;
    float     water_liters;
    int       battery_pct;
    float     battery_voltage;
    int       rssi;
    int       snr;
    int64_t   last_update_us;   // esp_timer_get_time() — 0 = never
    uint32_t  last_msg_id;
    uint32_t  packets_rx;
    bool      data_valid;
    tx_state_t state;
    char      fw_version[16];   // firmware version reported by transmitter

    // Power telemetry (from TX firmware v2.0.4 onward)
    char      power_mode;       // 'v'=voltage, 'i'=ina219, 'n'=disabled, '?'=unknown/old TX
    int32_t   current_ma;       // signed; +ve = discharging, -ve = charging
    int32_t   power_mw;         // V × I; 0 in voltage mode
    bool      charging;         // true if INA219 reports negative current

    // Sensor health: true when the TX's last TANK packet reported no valid
    // distance reading (dist=0 sentinel or out-of-range). The TX is alive
    // (last_update_us still fresh) but the ultrasonic sensor is failing —
    // bad echo, blocked surface, wrong sensor variant, etc. Water % and
    // liters are preserved from the previous good reading; UI should show
    // a "sensor error" badge so the user investigates rather than trusting
    // the (stale) reading.
    bool      sensor_error;

    // Variance-based safety net: defective JSN-SR04M / AJ-SR04M sensors
    // often don't fail outright — they report their ~20-25 cm min-range
    // as a constant value regardless of the actual water level. sensor_error
    // (Bug-A) doesn't catch this because the value looks plausible. We track
    // a 20-sample ring buffer of recent raw_dist readings; if spread (max-min)
    // ≤ 2 cm over a full window, the sensor is stuck and flagged.
    bool      sensor_stuck;

    // Sensor driver kind the TX last reported (since TX firmware v2.0.15).
    // RUNTIME ONLY — not persisted across reboots; comes fresh in every TANK
    // packet so the dashboard always shows "what the TX is actually running"
    // versus the registry's queued sensor_kind ("what the user requested").
    // Empty = old TX firmware that doesn't report it.
    char      active_sensor[12];
} tx_data_t;

// ── Registry init ─────────────────────────────────────────────────────────────
esp_err_t registry_init(void);

// ── CRUD ─────────────────────────────────────────────────────────────────────
int registry_add(uint16_t addr, const char *name,
                 int min_dist, int max_dist, float capacity);
bool registry_remove(uint16_t addr);
void registry_clear_all(void);
bool registry_update(uint16_t addr, const char *name,
                     int min_dist, int max_dist, float capacity);
bool registry_set_enabled(uint16_t addr, bool enabled);
bool registry_set_led_color(uint16_t addr, int8_t color_idx);

/** Set remote config parameters to be pushed to transmitter on next wake.
 *  pwr=0 means "don't change LoRa power"; 1-22 dBm sets it explicitly. */
bool registry_set_remote_config(uint16_t addr, uint32_t sleep_s, uint8_t samples, uint8_t pwr);

/** Set the TX's sensor driver kind ("sr04" or "ld2413"). NULL or empty clears
 *  the field. Always marks pending_config true so the next SET frame to this
 *  TX includes :SENSOR=<kind>. Returns false if addr unknown or kind invalid. */
bool registry_set_sensor_kind(uint16_t addr, const char *kind);

/** Check if pending config exists, fills outputs if yes. Does NOT clear flag.
 *  pwr_out is 0 if no pwr change requested. sensor_out is filled with the
 *  configured kind (empty string if never set) — pass NULL + 0 to skip. */
bool registry_get_pending_config(uint16_t addr,
                                 uint32_t *sleep_out, uint8_t *samples_out,
                                 uint8_t *pwr_out,
                                 char *sensor_out, size_t sensor_sz);

/** Clear pending config flag after SET_ACK is received from transmitter. */
bool registry_clear_pending_config(uint16_t addr);

/** Mark all transmitters as needing an OTA update. */
void registry_set_all_ota_pending(bool pending);

/** Get OTA progress for a specific transmitter. */
bool registry_get_ota_status(uint16_t addr, bool *pending_out, uint32_t *offset_out);

/** Update OTA progress for a specific transmitter (RAM only unless completing). */
void registry_set_ota_progress(uint16_t addr, uint32_t offset, bool pending);

/** Force-persist current registry state to SPIFFS (for periodic OTA checkpoints). */
void registry_persist(void);

/** Get SPIFFS partition usage. */
void registry_get_spiffs_info(size_t *total, size_t *used);

// ── Data update ──────────────────────────────────────────────────────────────
// `power_mode` is a single-char tag from the LoRa packet ('v'/'i'/'n'/'?').
// `current_ma`/`power_mw` are 0 when TX is in voltage mode or absent (old TX).
// `sensor_status` (since TX v2.0.12): 'o'=ok, 'e'=explicit sensor error,
// 'u'=unknown/old TX. Registry trusts the explicit 'o'/'e' signal; for 'u'
// it falls back to the dist==0 heuristic (Bug A safety net).
bool registry_update_data(uint16_t addr, int raw_dist, int bat_pct,
                           float bat_v, uint32_t msg_id, int rssi, int snr,
                           const char *fw_version,
                           char power_mode, int32_t current_ma, int32_t power_mw,
                           char sensor_status,
                           const char *active_sensor);

// ── History (Phase 2) ────────────────────────────────────────────────────────
/** Capture current readings into the half-hourly history buffer for any tank
 *  whose 30-min bucket has rolled over since its last write. Safe to call
 *  every minute — no-ops within the same bucket. `now` is wall-clock epoch
 *  seconds (skipped if 0 or before SNTP sync).
 *  Returns true if at least one slot was written. */
bool registry_history_tick(int64_t now);

/** Persist all history buffers to /spiffs/tx_hist.bin if dirty. Caller is
 *  expected to throttle to ~1 hour cadence. */
void registry_history_persist(void);

/** Retrieve a tank's 48-slot rolling history window.
 *  out_pct[i] / out_volt_dv[i] = oldest..newest, with TX_HIST_PCT_EMPTY /
 *  TX_HIST_VOLT_EMPTY for missing slots. *out_head_t = epoch seconds of the
 *  most-recent slot's bucket start. Returns false if idx invalid or no data. */
bool registry_get_history(int idx, uint8_t out_pct[TX_HIST_SLOTS],
                          uint8_t out_volt_dv[TX_HIST_SLOTS],
                          int64_t *out_head_t);

#ifdef DEBUG_HIST_INJECT
/** Test helper — overwrite the entire history buffer for a tank. Only compiled
 *  when DEBUG_HIST_INJECT is defined; lets the QA endpoint exercise the
 *  prediction code paths without a 24-hour soak. */
bool registry_history_inject(uint16_t addr, const uint8_t pct[TX_HIST_SLOTS],
                             const uint8_t volt_dv[TX_HIST_SLOTS],
                             int64_t head_t);
#endif

// ── Queries ──────────────────────────────────────────────────────────────────
int registry_count(void);
int registry_online_count(void);
int registry_find(uint16_t addr);
/** Look up an entry by its stable MAC identity. Returns the array index, or
 *  −1 if no entry has this MAC (including the all-zero "unknown" sentinel). */
int registry_find_by_mac(const uint8_t mac[6]);
/** Allocate the lowest free LoRa address in [1, 99] for a new pair.
 *  Reuses addresses freed by registry_remove. Returns 0 if exhausted (which
 *  is effectively never — 10-TX cap is far below the 99 ceiling). */
uint16_t registry_alloc_small_addr(void);
/** Attach a MAC to an existing entry. Useful when a returning TX sends MAC
 *  in PAIR_REQ but the registry entry was created before MAC was part of
 *  the protocol (legacy zero-MAC entry). Persists immediately. */
bool registry_set_mac(uint16_t addr, const uint8_t mac[6]);

/** Optional "registry membership changed" callback. mqtt_manager registers
 *  mqtt_publish_registry() here at init, so any add/remove/restore/clear
 *  triggers a fresh manifest publish without transmitter_registry needing to
 *  link against the MQTT component (avoids a circular dependency between
 *  mqtt_client and lora_rylr998). The callback fires AFTER the registry
 *  state has settled and AFTER any save_json(). Pass NULL to disable. */
typedef void (*registry_change_cb_t)(void);
void registry_set_change_callback(registry_change_cb_t cb);

/** Look up a soft-deleted (tombstoned) entry by MAC. registry_remove() moves
 *  entries to a tombstone archive instead of hard-deleting, so that a returning
 *  physical TX (same MAC) can be resurrected with its original name, address,
 *  alerts, capacity, etc. preserved. Returns the tombstone array index, or −1. */
int registry_archive_find_by_mac(const uint8_t mac[6]);

/** Resurrect a tombstoned entry back into the live registry. Removes it from
 *  the archive. The original LoRa address and all user-set config (name,
 *  capacity, alerts, sleep_s, lora_pwr) are preserved. History is NOT
 *  preserved in the tombstone — only the metadata. Returns false if the live
 *  registry is full or the tombstone index is invalid. */
bool registry_archive_restore(int tombstone_idx);

bool registry_get_info(int idx, tx_info_t *out);
bool registry_get_data(int idx, tx_data_t *out);
tx_state_t registry_worst_state(void);
void registry_update_states(void);
void registry_sanitize_name(const char *name, char *out, size_t out_len);
const char *registry_state_str(tx_state_t state);
