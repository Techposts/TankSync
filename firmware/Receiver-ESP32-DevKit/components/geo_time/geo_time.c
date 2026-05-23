// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Ravi Singh / SmartGhar

#include "geo_time.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "geo";

// 3-byte buffer: 2-letter ISO code + NUL. "??" until detected.
static char s_country[3] = "XX";
static bool s_time_synced = false;
static bool s_inited = false;

#define NVS_NS     "geo"
#define NVS_KEY    "cc"
#define NVS_KEY_TZ "tz"

#define TZ_MAX 64
static char s_tz[TZ_MAX] = "UTC0";

// ── Country → POSIX TZ default table ──────────────────────────────────────
// For single-TZ countries this is the correct zone; for multi-TZ countries
// it is the most-populous-zone fallback (US→Eastern, AU→Sydney, CA→Eastern,
// RU→Moscow, BR→Brasília, MX→Central). User can override via geo_time_set_tz.
// Format: POSIX TZ string per IEEE 1003.1 / glibc rules.
typedef struct { const char *cc; const char *tz; } cc_tz_t;
static const cc_tz_t CC_TZ[] = {
    // South Asia
    {"IN", "IST-5:30"},
    {"BD", "BDT-6"},
    {"LK", "<+0530>-5:30"},
    {"NP", "<+0545>-5:45"},
    {"PK", "PKT-5"},
    // Middle East
    {"AE", "<+04>-4"},
    {"SA", "<+03>-3"},
    {"IL", "IST-2IDT,M3.4.4/26,M10.5.0"},
    // East Asia
    {"JP", "JST-9"},
    {"KR", "KST-9"},
    {"CN", "CST-8"},
    {"HK", "HKT-8"},
    {"TW", "CST-8"},
    {"SG", "<+08>-8"},
    {"MY", "<+08>-8"},
    {"TH", "<+07>-7"},
    {"VN", "<+07>-7"},
    {"ID", "WIB-7"},
    {"PH", "PST-8"},
    // Europe — UK / Ireland (DST)
    {"GB", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"IE", "GMT0IST,M3.5.0/1,M10.5.0"},
    {"PT", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"IS", "GMT0"},
    // Europe — Central (DST)
    {"DE", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"FR", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"IT", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"NL", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"BE", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"ES", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"AT", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"CH", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"DK", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"SE", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"NO", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"PL", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"CZ", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"HU", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"SK", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"SI", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"HR", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"LU", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"MT", "CET-1CEST,M3.5.0,M10.5.0/3"},
    // Europe — Eastern (DST)
    {"FI", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"GR", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"BG", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"RO", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"LT", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"LV", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"EE", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"CY", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    // CIS — fallback to Moscow
    {"RU", "MSK-3"},
    {"UA", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"TR", "<+03>-3"},
    // Africa
    {"ZA", "SAST-2"},
    {"EG", "EET-2"},
    {"NG", "WAT-1"},
    {"KE", "EAT-3"},
    // Americas — North (multi-TZ countries, picking most-populous fallback)
    {"US", "EST5EDT,M3.2.0,M11.1.0"},
    {"CA", "EST5EDT,M3.2.0,M11.1.0"},
    {"MX", "CST6CDT,M4.1.0,M10.5.0"},
    // Americas — Central / Caribbean
    {"GT", "CST6"},
    {"CR", "CST6"},
    {"PA", "EST5"},
    // Americas — South
    {"BR", "<-03>3"},
    {"AR", "<-03>3"},
    {"CL", "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
    {"CO", "COT5"},
    {"PE", "PET5"},
    {"VE", "<-04>4"},
    // Oceania
    {"AU", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"NZ", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};
#define CC_TZ_COUNT (sizeof(CC_TZ)/sizeof(CC_TZ[0]))

// ── HTTP fetch handler — collects response body into a small buffer ────────
typedef struct { char buf[128]; int len; } http_buf_t;

static esp_err_t http_evt(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
        http_buf_t *b = (http_buf_t *)evt->user_data;
        int copy = evt->data_len;
        if (b->len + copy >= (int)sizeof(b->buf)) copy = sizeof(b->buf) - 1 - b->len;
        if (copy > 0) {
            memcpy(b->buf + b->len, evt->data, copy);
            b->len += copy;
            b->buf[b->len] = '\0';
        }
    }
    return ESP_OK;
}

static bool detect_country_via_http(char out[3]) {
    http_buf_t body = {0};
    esp_http_client_config_t cfg = {
        .url = "http://ip-api.com/json/?fields=countryCode",
        .event_handler = http_evt,
        .user_data = &body,
        .timeout_ms = 6000,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;
    esp_err_t err = esp_http_client_perform(cli);
    int code = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    if (err != ESP_OK || code != 200 || body.len < 5) {
        ESP_LOGW(TAG, "ip-api fetch failed (err=%s status=%d body=%d)",
                 esp_err_to_name(err), code, body.len);
        return false;
    }
    cJSON *j = cJSON_Parse(body.buf);
    if (!j) return false;
    const char *cc = cJSON_GetStringValue(cJSON_GetObjectItem(j, "countryCode"));
    bool ok = false;
    if (cc && strlen(cc) == 2) {
        out[0] = cc[0]; out[1] = cc[1]; out[2] = 0;
        ok = true;
    }
    cJSON_Delete(j);
    return ok;
}

static void load_country_from_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_country);
    if (nvs_get_str(h, NVS_KEY, s_country, &len) != ESP_OK) {
        // not set yet
        s_country[0] = 'X'; s_country[1] = 'X'; s_country[2] = 0;
    }
    nvs_close(h);
}

static void save_country_to_nvs(const char *cc) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY, cc);
    nvs_commit(h);
    nvs_close(h);
}

// ── TZ persistence + apply ─────────────────────────────────────────────────
static void apply_tz(const char *tz) {
    if (!tz || !*tz) tz = "UTC0";
    strncpy(s_tz, tz, TZ_MAX - 1);
    s_tz[TZ_MAX - 1] = '\0';
    setenv("TZ", s_tz, 1);
    tzset();
    ESP_LOGI(TAG, "TZ applied: %s", s_tz);
}

static bool load_tz_from_nvs(char out[TZ_MAX]) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = TZ_MAX;
    bool ok = (nvs_get_str(h, NVS_KEY_TZ, out, &len) == ESP_OK && len > 0);
    nvs_close(h);
    return ok;
}

static void save_tz_to_nvs(const char *tz) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (tz && *tz) nvs_set_str(h, NVS_KEY_TZ, tz);
    else           nvs_erase_key(h, NVS_KEY_TZ);
    nvs_commit(h);
    nvs_close(h);
}

// ── SNTP callback ──────────────────────────────────────────────────────────
static void on_sntp_sync(struct timeval *tv) {
    s_time_synced = true;
    time_t now = tv->tv_sec;
    struct tm tm; localtime_r(&now, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    ESP_LOGI(TAG, "SNTP synced — %s UTC", buf);
}

// Apply the best TZ we know right now: explicit NVS override → CC default → UTC.
static void apply_best_tz(void) {
    char nvs_tz[TZ_MAX];
    if (load_tz_from_nvs(nvs_tz)) { apply_tz(nvs_tz); return; }
    const char *suggested = geo_suggested_tz(s_country);
    apply_tz(suggested ? suggested : "UTC0");
}

// ── Background task: wait for WiFi GOT_IP, then SNTP + geo detect ──────────
static void geo_task(void *arg) {
    (void)arg;
    // Apply best TZ we have BEFORE WiFi — even if it's UTC0, downstream
    // localtime_r() callers (buzzer, OLED, history) get a stable answer.
    load_country_from_nvs();
    apply_best_tz();

    // Wait until WiFi has an IP. Poll netif each second; cheap on power.
    for (;;) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip;
        if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "WiFi up — starting SNTP + country detect");

    // SNTP — non-blocking, callback fires once synced
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(on_sntp_sync);
    esp_sntp_init();

    // If we already have a country code in NVS, TZ is already applied above.
    if (s_country[0] != 'X') {
        ESP_LOGI(TAG, "Country (cached): %s", s_country);
        vTaskDelete(NULL);
        return;
    }
    // Give the network stack a moment, then try ip-api.com.
    vTaskDelay(pdMS_TO_TICKS(2000));
    char cc[3];
    if (detect_country_via_http(cc)) {
        memcpy(s_country, cc, 3);
        save_country_to_nvs(cc);
        ESP_LOGI(TAG, "Country (detected): %s", s_country);
        // Re-derive TZ from the freshly detected country (unless user
        // already explicitly set one via geo_time_set_tz).
        char nvs_tz[TZ_MAX];
        if (!load_tz_from_nvs(nvs_tz)) {
            const char *suggested = geo_suggested_tz(s_country);
            if (suggested) apply_tz(suggested);
        }
    } else {
        ESP_LOGW(TAG, "Country detect failed; staying unknown");
    }
    vTaskDelete(NULL);
}

esp_err_t geo_time_init(void) {
    if (s_inited) return ESP_OK;
    s_inited = true;
    BaseType_t r = xTaskCreate(geo_task, "geo", 4096, NULL, 3, NULL);
    return r == pdPASS ? ESP_OK : ESP_FAIL;
}

const char *geo_get_country(void) { return s_country; }
bool geo_time_is_synced(void) { return s_time_synced; }

void geo_time_get_tz(char *out, size_t cap) {
    if (!out || cap == 0) return;
    strncpy(out, s_tz, cap - 1);
    out[cap - 1] = '\0';
}

esp_err_t geo_time_set_tz(const char *tz) {
    save_tz_to_nvs(tz);
    if (tz && *tz) {
        apply_tz(tz);
    } else {
        // Cleared override — fall back to CC default or UTC0.
        const char *suggested = geo_suggested_tz(s_country);
        apply_tz(suggested ? suggested : "UTC0");
    }
    return ESP_OK;
}

const char *geo_suggested_tz(const char *cc) {
    if (!cc || cc[0] == 'X' || strlen(cc) != 2) return NULL;
    for (size_t i = 0; i < CC_TZ_COUNT; i++) {
        if (!strcmp(cc, CC_TZ[i].cc)) return CC_TZ[i].tz;
    }
    return NULL;
}

int geo_suggested_freq_mhz(const char *cc) {
    if (!cc || cc[0] == 'X' || strlen(cc) != 2) return 0;
    // India / South Asia / parts of Africa: 865 MHz band
    if (!strcmp(cc, "IN") || !strcmp(cc, "BD") || !strcmp(cc, "LK") || !strcmp(cc, "NP")) return 865;
    // EU + GB + most of Europe: 868 MHz band
    static const char *eu[] = {"DE","FR","IT","GB","NL","BE","ES","PT","AT","CH","DK","SE","NO","FI","IE","PL","CZ","HU","RO","BG","GR","SK","SI","HR","LT","LV","EE","LU","MT","CY","IS",NULL};
    for (int i = 0; eu[i]; i++) if (!strcmp(cc, eu[i])) return 868;
    // North America + Australia + NZ: 915 MHz band
    static const char *us[] = {"US","CA","MX","AU","NZ","BR","AR","CL",NULL};
    for (int i = 0; us[i]; i++) if (!strcmp(cc, us[i])) return 915;
    // Japan: 920 MHz
    if (!strcmp(cc, "JP")) return 920;
    return 0;  // unknown — UI hides the compliance hint
}
