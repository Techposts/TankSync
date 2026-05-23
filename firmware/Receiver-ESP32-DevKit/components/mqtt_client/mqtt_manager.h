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
 *   tanksync/{device_id}/system/version      = "2.1.10"
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
#define FIRMWARE_VERSION        "2.8.4"
#endif

// LED slot layout (canonical defs in main/config.h). Fallbacks here so the
// mqtt_client component's identify command handlers compile in isolation.
#ifndef LED_IDX_STATUS
#define LED_IDX_STATUS      0
#endif
#ifndef LED_IDX_TANK_START
#define LED_IDX_TANK_START  2
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

/** Publish the hub's current transmitter registry as a retained JSON message
 *  to `tanksync/<id>/registry/devices`. Cloud subscribers use this as the
 *  source of truth for which TXs are currently paired — any cloud DB row
 *  whose lora_address isn't in this list is an orphan and gets pruned by
 *  the cloud's reconcile loop. Safe to call anywhere; no-op if MQTT is
 *  disconnected. Idempotent. */
void mqtt_publish_registry(void);

/** Clear ALL retained MQTT topics for a transmitter that's been removed from
 *  the registry. Without this, the broker keeps the last-known retained
 *  values (water_pct, state, name, capacity, etc.) forever. Publishes empty
 *  payload + retain=1 to each retained field mqtt_publish_tank emits. Call
 *  this BEFORE registry_remove() shifts the array so the addr is still valid
 *  in the registry when we look it up. Safe to call when MQTT is down (no-op). */
void mqtt_unpublish_tank(uint16_t addr);

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

/**
 * True if this device has cloud credentials provisioned (NVS has user+pass and
 * MQTT is enabled). Reflects "claimed" state — does NOT flicker during transient
 * reconnects. UI uses this to decide whether to show the QR / setup flow.
 */
bool mqtt_manager_is_linked(void);
