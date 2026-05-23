/**
 * geo_time — wall-clock time (SNTP) + country detection (HTTP IP geolocation)
 *
 * Spawns a background task on init that waits for WiFi, then:
 *   1. Starts SNTP (pool.ntp.org). Wall-clock time becomes available within
 *      ~5 sec. Used for the future hourly history bucket alignment, "last
 *      seen" timestamps, and OLED clock display.
 *   2. Reads NVS "geo"/"cc" — if set, skips step 3.
 *   3. Issues a one-shot HTTP GET to http://ip-api.com/json/?fields=countryCode
 *      Parses {"countryCode":"IN"}, persists in NVS.
 *
 * Country code is used for LoRa-frequency compliance hints in the web UI —
 * never to auto-change the radio settings. The user retains control.
 *
 * If ip-api.com is unreachable or returns garbage, country defaults to "??"
 * and the compliance hint stays hidden. SNTP is independent and keeps trying.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/** Initialize SNTP + country-detect background task. Idempotent. Call once
 *  after wifi_manager_init(). Returns ESP_OK if the task was spawned (the
 *  task itself blocks on WIFI_GOT_IP). */
esp_err_t geo_time_init(void);

/** Return the cached two-letter country code (e.g. "IN", "DE", "US") or "??"
 *  if not yet detected. Always returns a valid 3-byte buffer (2 chars + NUL). */
const char *geo_get_country(void);

/** Return the suggested LoRa frequency in MHz for a given country code, or 0
 *  if unknown. Reference table: India 865, EU 868, US/CA 915, AU 915, JP 920. */
int geo_suggested_freq_mhz(const char *country_code);

/** True when SNTP has synced at least once since boot. */
bool geo_time_is_synced(void);

/** Copy the active POSIX TZ string (e.g. "IST-5:30" or "EST5EDT,M3.2.0,M11.1.0")
 *  into `out`. Always NUL-terminated. Defaults to "UTC0" if unset. `cap` should
 *  be at least 48 to comfortably hold any standard zone string. */
void geo_time_get_tz(char *out, size_t cap);

/** Persist a POSIX TZ string to NVS, then apply via setenv("TZ",...)+tzset().
 *  Empty / NULL clears the override and re-derives from country code on next boot.
 *  After this call, localtime_r() returns local time hub-wide. */
esp_err_t geo_time_set_tz(const char *tz);

/** Return the suggested POSIX TZ string for a country code, or NULL if no
 *  default is known. For multi-TZ countries we return the most-populous-zone
 *  fallback (e.g. US→Eastern, AU→Sydney). User can override via geo_time_set_tz. */
const char *geo_suggested_tz(const char *country_code);
