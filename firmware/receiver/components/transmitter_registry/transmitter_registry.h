// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

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
#define TX_NAME_MAX         16

// ── Persistent configuration per transmitter ─────────────────────────────────
typedef struct {
    uint16_t address;               // LoRa address (1-65535)
    char     name[TX_NAME_MAX];     // Friendly name, e.g. "Main Tank"
    int      min_dist_cm;           // Sensor reading when tank is FULL
    int      max_dist_cm;           // Sensor reading when tank is EMPTY
    float    capacity_liters;       // Total tank volume
    bool     enabled;
    // Remote config fields
    uint32_t sleep_s;               // Sleep interval in seconds
    uint8_t  samples;               // Number of sensor samples
    bool     pending_config;        // true if config needs to be sent
    // LoRa OTA fields
    bool     ota_pending;           // true if firmware update queued
    uint32_t ota_offset;            // Current byte offset in transfer
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

/** Set remote config parameters to be pushed to transmitter on next wake. */
bool registry_set_remote_config(uint16_t addr, uint32_t sleep_s, uint8_t samples);

/** Check if pending config exists, fills output if yes. Does NOT clear flag. */
bool registry_get_pending_config(uint16_t addr, uint32_t *sleep_out, uint8_t *samples_out);

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
bool registry_update_data(uint16_t addr, int raw_dist, int bat_pct,
                           float bat_v, uint32_t msg_id, int rssi, int snr,
                           const char *fw_version);

// ── Queries ──────────────────────────────────────────────────────────────────
int registry_count(void);
int registry_online_count(void);
int registry_find(uint16_t addr);
bool registry_get_info(int idx, tx_info_t *out);
bool registry_get_data(int idx, tx_data_t *out);
tx_state_t registry_worst_state(void);
void registry_update_states(void);
void registry_sanitize_name(const char *name, char *out, size_t out_len);
const char *registry_state_str(tx_state_t state);
