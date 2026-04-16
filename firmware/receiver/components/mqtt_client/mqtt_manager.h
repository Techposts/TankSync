// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

/**
 * mqtt_manager - MQTT publishing for TankSync receiver
 *
 * Topics (all retained where noted):
 *   tanksync/{device_id}/status              = "online" / "offline" (LWT, retained)
 *   tanksync/{device_id}/{tank}/water_pct    = 0-100
 *   tanksync/{device_id}/{tank}/water_liters = float
 *   tanksync/{device_id}/{tank}/distance_cm  = int
 *   tanksync/{device_id}/{tank}/battery_pct  = int
 *   tanksync/{device_id}/{tank}/battery_v    = float
 *   tanksync/{device_id}/{tank}/rssi         = int
 *   tanksync/{device_id}/{tank}/state        = "online"|"stale"|"offline"|"waiting"
 *   tanksync/{device_id}/system/ip           = "x.x.x.x"
 *   tanksync/{device_id}/system/version      = "2.0.0"
 *   tanksync/{device_id}/system/uptime       = seconds
 *
 * HA Discovery (optional):
 *   homeassistant/sensor/tanksync_{device_id}/{tank}_{field}/config
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// MQTT defaults
#ifndef MQTT_DEFAULT_PORT
#define MQTT_DEFAULT_PORT       1883
#endif
#ifndef MQTT_TOPIC_PREFIX
#define MQTT_TOPIC_PREFIX       "tanksync"
#endif
#ifndef MQTT_KEEPALIVE_S
#define MQTT_KEEPALIVE_S        60
#endif
#ifndef MQTT_RECONNECT_BASE_MS
#define MQTT_RECONNECT_BASE_MS  5000
#endif
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION        "2.1.0"
#endif

typedef enum {
    MQTT_ST_DISABLED = 0,
    MQTT_ST_CONNECTING,
    MQTT_ST_CONNECTED,
    MQTT_ST_DISCONNECTED,
    MQTT_ST_ERROR,
} mqtt_mgr_status_t;

typedef struct {
    char    host[128];
    uint16_t port;
    char    user[64];
    char    pass[64];
    bool    enabled;
    bool    ha_discovery;
} mqtt_mgr_config_t;

/** Initialize: load config from NVS, derive device_id from MAC. */
esp_err_t mqtt_manager_init(void);

/** Start connection. Call after WiFi connects. No-op if disabled. */
void mqtt_manager_start(void);

/** Publish "offline" LWT then disconnect. */
void mqtt_manager_stop(void);

/** Publish all topics for transmitter at registry index idx. */
void mqtt_publish_tank(int idx);

/** Publish system info (IP, version, uptime, wifi RSSI). */
void mqtt_publish_system(void);

/** Publish HA MQTT Discovery config for all enabled transmitters. */
void mqtt_publish_ha_discovery(void);

/** Current connection status. */
mqtt_mgr_status_t mqtt_manager_status(void);

/** Copy current config (password field is blanked). */
void mqtt_manager_get_config(mqtt_mgr_config_t *out);

/** Save config to NVS and reconnect. */
esp_err_t mqtt_manager_set_config(const mqtt_mgr_config_t *cfg);

/** 12-char hex device ID derived from WiFi MAC. */
const char *mqtt_manager_device_id(void);
