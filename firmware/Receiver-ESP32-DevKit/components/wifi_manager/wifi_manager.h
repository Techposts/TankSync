/**
 * wifi_manager - WiFi STA + AP fallback, mDNS, captive portal
 *
 * Behavior:
 *   1. If credentials saved in NVS → connect in STA mode
 *   2. On failure / no credentials → start AP "TankSync" + captive portal
 *   3. mDNS: tanksync.local → device IP
 *   4. Captive portal: simple DNS server resolves all to 192.168.4.1
 *
 * Events are posted to the system event group (EVT_WIFI_*).
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

typedef enum {
    WIFI_ST_DISCONNECTED = 0,
    WIFI_ST_CONNECTING,
    WIFI_ST_CONNECTED,
    WIFI_ST_AP_MODE,
} wifi_status_t;

/**
 * Initialize WiFi subsystem. Call once from app_main.
 * @param events  System event group to post EVT_WIFI_* bits to.
 */
esp_err_t wifi_manager_init(EventGroupHandle_t events);

/** Start a WiFi connection attempt (non-blocking). */
void wifi_manager_connect(void);

/** Start AP mode (captive portal). */
void wifi_manager_start_ap(void);

/** Save credentials to NVS and reconnect. */
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);

/** Clear saved credentials and restart in AP mode. */
esp_err_t wifi_manager_forget(void);

/** Get current status. */
wifi_status_t wifi_manager_status(void);

/** Get device IP as string (STA IP or "192.168.4.1" in AP mode). */
const char *wifi_manager_ip(void);

/** Get SSID of connected network (or AP SSID). */
const char *wifi_manager_ssid(void);

/** Get WiFi RSSI in STA mode (0 if not connected). */
int wifi_manager_rssi(void);

/** Get mDNS hostname (e.g. "tanksync-a1b2"). */
const char *wifi_manager_mdns_host(void);

/** Get globally-unique hub identifier (12-char lowercase hex MAC).
 *  Stable across reboots, used as the SmartGhar-protocol hub_id. */
const char *wifi_manager_hub_id(void);

// Canonical firmware version lives in main/config.h. This fallback keeps
// component-only builds working when main/config.h isn't on the include path.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "2.8.4"
#endif

/** Scan for nearby networks. Returns JSON array string (caller must free). */
char *wifi_manager_scan_json(void);
