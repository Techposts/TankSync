// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

/**
 * web_server - HTTP dashboard + REST API for TankSync receiver
 *
 * Endpoints:
 *   GET  /                      → Dashboard HTML
 *   GET  /api/data              → All tank data as JSON
 *   GET  /api/system            → System info (version, IP, uptime, WiFi)
 *   GET  /api/transmitters      → Transmitter list
 *   POST /api/transmitters      → Add transmitter  {addr, name, min_dist, max_dist, capacity}
 *   PUT  /api/transmitters/{addr} → Update transmitter
 *   DELETE /api/transmitters/{addr} → Remove transmitter
 *   GET  /api/wifi/scan         → Scan WiFi networks → JSON array
 *   POST /api/wifi/connect      → {ssid, password}
 *   POST /api/wifi/forget       → Clear credentials
 *   GET  /api/mqtt              → MQTT config (password masked)
 *   POST /api/mqtt              → Save MQTT config
 *   GET  /api/lora              → LoRa config (addr, netid, freq, sf, bw, power)
 *   POST /api/lora              → Save LoRa config
 *   GET  /api/ota/state         → OTA status JSON
 *   POST /api/ota/check         → Trigger GitHub check
 *   POST /api/ota/update        → Flash latest GitHub release
 *   POST /api/ota/upload        → Upload .bin (multipart or raw POST body)
 */

#pragma once

#include "esp_err.h"

#ifndef WEB_PORT
#define WEB_PORT        80
#endif
#ifndef WEB_MAX_SOCKETS
#define WEB_MAX_SOCKETS 5
#endif
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "2.1.0"
#endif
#ifndef TANKSYNC_CLOUD_URL
#define TANKSYNC_CLOUD_URL "https://your-tanksync-server.example.com"
#endif

/**
 * Start the HTTP server on port WEB_PORT (80).
 * Call after nvs_flash_init and wifi_manager_init.
 */
esp_err_t web_server_start(void);

/** Stop the HTTP server. */
void web_server_stop(void);
