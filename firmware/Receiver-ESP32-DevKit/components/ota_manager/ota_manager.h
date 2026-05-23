/**
 * ota_manager - OTA firmware updates for TankSync receiver
 *
 * Two update paths:
 *   1. Cloud manifest: auto-check every 24h + manual trigger
 *      GET https://tanksync.smartghar.org/api/firmware/latest?target=rx
 *      Server proxies GitHub releases of private tanksync-cloud repo using
 *      a server-side PAT. Returns GH-shape JSON; firmware needs no parser
 *      changes. Downloads asset matching "tanksync-receiver-rx-v*.bin".
 *
 *   2. Manual upload: caller provides a URL to a .bin file
 *      (web_server passes the HTTP upload URL to ota_manager_flash_url)
 *
 * Rollback safety: new firmware must call ota_manager_mark_valid() on
 * first successful boot or the bootloader will revert to previous image.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

// OTA configuration defaults (canonical values live in main/config.h)
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION        "2.8.4"
#endif
#ifndef OTA_MANIFEST_URL
#define OTA_MANIFEST_URL        "https://tanksync.smartghar.org/api/firmware/latest?target=rx"
#endif
#ifndef OTA_CHECK_INTERVAL_H
#define OTA_CHECK_INTERVAL_H    24
#endif
#ifndef OTA_ASSET_PREFIX
#define OTA_ASSET_PREFIX        "tanksync-receiver-rx-v"
#endif
#ifndef OTA_ASSET_SUFFIX
#define OTA_ASSET_SUFFIX        ".bin"
#endif

typedef enum {
    OTA_ST_IDLE = 0,
    OTA_ST_CHECKING,        // Querying GitHub API
    OTA_ST_AVAILABLE,       // Newer version found
    OTA_ST_DOWNLOADING,     // Downloading + flashing
    OTA_ST_DONE,            // Flash complete, pending reboot
    OTA_ST_UP_TO_DATE,      // Already on latest
    OTA_ST_ERROR,
} ota_status_t;

typedef struct {
    ota_status_t status;
    char latest_version[32];    // e.g. "2.1.10" (from GitHub tag, strip "v" prefix)
    char download_url[256];
    int  progress_pct;          // 0-100 during download
    char error_msg[128];
} ota_state_t;

/**
 * Initialize OTA manager.
 * Marks current firmware as valid (rollback protection).
 * Starts background task that checks GitHub every OTA_CHECK_INTERVAL_H hours.
 */
esp_err_t ota_manager_init(void);

/**
 * Trigger an immediate manifest check (non-blocking, runs in background).
 * Returns ESP_ERR_INVALID_STATE if a check or download is already in
 * progress (callers should surface a "please wait" message). On success,
 * result is available via ota_manager_get_state() within ~60s.
 */
esp_err_t ota_manager_check_github(void);

/**
 * Download and flash firmware from a URL (blocking, run in OTA task).
 * url: direct .bin download URL (GitHub asset, or any HTTPS URL)
 * Returns ESP_OK on successful flash (device will reboot).
 */
esp_err_t ota_manager_flash_url(const char *url);

/**
 * Mark current firmware as valid (call once after successful startup).
 * Prevents rollback on next boot. Safe to call multiple times.
 */
void ota_manager_mark_valid(void);

/** Get current OTA state (copy, safe to call from any task). */
void ota_manager_get_state(ota_state_t *out);

/** True if an update is currently in progress. */
bool ota_manager_busy(void);

/**
 * Non-blocking install trigger. Spawns a one-shot task that runs the
 * blocking ota_manager_flash_url(state.download_url). Safe to call from
 * MQTT command handler — would otherwise stall the MQTT event loop for
 * the ~30s flash duration.
 *
 * Returns:
 *   ESP_OK              — task spawned, install in progress
 *   ESP_ERR_INVALID_STATE — already busy, or no download_url cached
 *                           (caller must run ota_manager_check_github first)
 *   ESP_ERR_NO_MEM      — task creation failed
 */
esp_err_t ota_manager_request_install(void);
