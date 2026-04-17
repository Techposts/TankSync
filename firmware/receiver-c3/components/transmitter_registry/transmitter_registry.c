// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

/**
 * transmitter_registry implementation
 * Persistence: SPIFFS /transmitters.json (cJSON)
 * Thread safety: single mutex wraps all access to s_infos / s_data
 */

#include "transmitter_registry.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "registry";

// Strip non-printable-ASCII and chars that break HTML/JS string literals.
// Keeps A-Z, a-z, 0-9, space, hyphen, underscore.
static void sanitize_name(char *dst, const char *src, size_t maxlen) {
    size_t j = 0;
    if (!src) { strncpy(dst, "Tank", maxlen - 1); dst[maxlen-1] = '\0'; return; }
    for (size_t i = 0; src[i] && j < maxlen - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == ' ' || c == '-' || c == '_') {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
    if (j == 0) { strncpy(dst, "Tank", maxlen - 1); dst[maxlen - 1] = '\0'; }
}
static const char *JSON_PATH = "/spiffs/transmitters.json";

// Data-stale thresholds (match config.h)
#define STALE_US    (600LL * 1000000LL)     // 10 min
#define LOST_US     (900LL * 1000000LL)     // 15 min

// ── Internal storage ──────────────────────────────────────────────────────────
static tx_info_t        s_infos[MAX_TRANSMITTERS];
static tx_data_t        s_data [MAX_TRANSMITTERS];
static int              s_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

#define LOCK()    xSemaphoreTake(s_mutex, portMAX_DELAY)
#define UNLOCK()  xSemaphoreGive(s_mutex)

// ── Water level calculation ───────────────────────────────────────────────────
static void calc_water(int idx) {
    tx_info_t *info = &s_infos[idx];
    tx_data_t *data = &s_data[idx];

    int range = info->max_dist_cm - info->min_dist_cm;
    if (range <= 0) { data->water_pct = 0; data->water_liters = 0; return; }

    int level = info->max_dist_cm - data->raw_dist_cm;
    if (level < 0)     level = 0;
    if (level > range) level = range;

    data->water_pct    = (level * 100) / range;
    data->water_liters = (info->capacity_liters * level) / range;
}

// ── SPIFFS JSON persistence ───────────────────────────────────────────────────
static bool save_json(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 2);
    cJSON *arr = cJSON_AddArrayToObject(root, "transmitters");

    for (int i = 0; i < s_count; i++) {
        cJSON *tx = cJSON_CreateObject();
        cJSON_AddNumberToObject(tx, "addr",     s_infos[i].address);
        cJSON_AddStringToObject(tx, "name",     s_infos[i].name);
        cJSON_AddNumberToObject(tx, "min_dist", s_infos[i].min_dist_cm);
        cJSON_AddNumberToObject(tx, "max_dist", s_infos[i].max_dist_cm);
        cJSON_AddNumberToObject(tx, "capacity", s_infos[i].capacity_liters);
        cJSON_AddBoolToObject  (tx, "enabled",  s_infos[i].enabled);
        cJSON_AddNumberToObject(tx, "sleep",    s_infos[i].sleep_s);
        cJSON_AddNumberToObject(tx, "samples",  s_infos[i].samples);
        cJSON_AddBoolToObject  (tx, "pending",  s_infos[i].pending_config);
        cJSON_AddBoolToObject  (tx, "ota_p",    s_infos[i].ota_pending);
        cJSON_AddNumberToObject(tx, "ota_o",    s_infos[i].ota_offset);
        // Persist last-known firmware version (survives receiver reboot)
        if (s_data[i].fw_version[0])
            cJSON_AddStringToObject(tx, "fw_ver", s_data[i].fw_version);
        cJSON_AddItemToArray(arr, tx);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return false;

    FILE *f = fopen(JSON_PATH, "w");
    bool ok = false;
    if (f) {
        fputs(json_str, f);
        fclose(f);
        ok = true;
        ESP_LOGI(TAG, "Saved %d transmitters (%d bytes)", s_count, (int)strlen(json_str));
    } else {
        ESP_LOGE(TAG, "Cannot open %s for write", JSON_PATH);
    }
    free(json_str);
    return ok;
}

static bool load_json(void) {
    FILE *f = fopen(JSON_PATH, "r");
    if (!f) { ESP_LOGW(TAG, "No transmitters.json"); return false; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 8192) { fclose(f); return false; }

    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return false; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { ESP_LOGE(TAG, "JSON parse error"); return false; }

    cJSON *arr = cJSON_GetObjectItem(root, "transmitters");
    s_count = 0;
    cJSON *tx;
    cJSON_ArrayForEach(tx, arr) {
        if (s_count >= MAX_TRANSMITTERS) break;
        tx_info_t *info = &s_infos[s_count];
        info->address         = (uint16_t)cJSON_GetNumberValue(cJSON_GetObjectItem(tx, "addr"));
        const char *name      = cJSON_GetStringValue(cJSON_GetObjectItem(tx, "name"));
        info->min_dist_cm     = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(tx, "min_dist"));
        info->max_dist_cm     = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(tx, "max_dist"));
        info->capacity_liters = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(tx, "capacity"));
        cJSON *en             = cJSON_GetObjectItem(tx, "enabled");
        info->enabled         = en ? cJSON_IsTrue(en) : true;
        sanitize_name(info->name, name ? name : "Tank", TX_NAME_MAX);

        // Remote config with defaults
        cJSON *sl = cJSON_GetObjectItem(tx, "sleep");
        info->sleep_s = sl ? (uint32_t)cJSON_GetNumberValue(sl) : 300;
        cJSON *sa = cJSON_GetObjectItem(tx, "samples");
        info->samples = sa ? (uint8_t)cJSON_GetNumberValue(sa) : 5;
        cJSON *pe = cJSON_GetObjectItem(tx, "pending");
        info->pending_config = pe ? cJSON_IsTrue(pe) : false;
        cJSON *op = cJSON_GetObjectItem(tx, "ota_p");
        info->ota_pending = op ? cJSON_IsTrue(op) : false;
        cJSON *oo = cJSON_GetObjectItem(tx, "ota_o");
        info->ota_offset = oo ? (uint32_t)cJSON_GetNumberValue(oo) : 0;

        // Init runtime data
        s_data[s_count].address = info->address;
        s_data[s_count].state   = TX_STATE_WAITING;
        // Restore last-known firmware version
        const char *fw = cJSON_GetStringValue(cJSON_GetObjectItem(tx, "fw_ver"));
        if (fw && fw[0]) {
            strncpy(s_data[s_count].fw_version, fw, sizeof(s_data[s_count].fw_version) - 1);
            s_data[s_count].fw_version[sizeof(s_data[s_count].fw_version) - 1] = '\0';
        }
        s_count++;
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d transmitter(s)", s_count);
    return s_count > 0;
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t registry_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    // Mount SPIFFS
    esp_vfs_spiffs_conf_t spiffs_cfg = {
        .base_path              = "/spiffs",
        .partition_label        = NULL,
        .max_files              = 5,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&spiffs_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    // Log SPIFFS usage for diagnostics
    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS: %d/%d bytes used (%.0f%% full, %dKB free)",
             (int)used, (int)total,
             total ? 100.0 * used / total : 0,
             (int)((total - used) / 1024));

    memset(s_infos, 0, sizeof(s_infos));
    memset(s_data,  0, sizeof(s_data));

    load_json(); // Load existing, or start empty if none
    return ESP_OK;
}

int registry_add(uint16_t addr, const char *name,
                 int min_dist, int max_dist, float capacity) {
    LOCK();
    // Check duplicate
    for (int i = 0; i < s_count; i++) {
        if (s_infos[i].address == addr) { UNLOCK(); return -1; }
    }
    if (s_count >= MAX_TRANSMITTERS) { UNLOCK(); return -1; }

    int idx = s_count;
    s_infos[idx].address         = addr;
    s_infos[idx].min_dist_cm     = min_dist > 0 ? min_dist : 30;
    s_infos[idx].max_dist_cm     = max_dist > 0 ? max_dist : 120;
    s_infos[idx].capacity_liters = capacity > 0 ? capacity : 942.5f;
    s_infos[idx].enabled         = true;
    s_infos[idx].sleep_s         = 300;
    s_infos[idx].samples         = 5;
    s_infos[idx].pending_config  = false;
    s_infos[idx].ota_pending     = false;
    s_infos[idx].ota_offset      = 0;
    sanitize_name(s_infos[idx].name, name ? name : "Tank", TX_NAME_MAX);

    s_data[idx].address = addr;
    s_data[idx].state   = TX_STATE_WAITING;
    s_count++;

    save_json();
    UNLOCK();
    ESP_LOGI(TAG, "Added addr=%d name='%s'", addr, s_infos[idx].name);
    return idx;
}

bool registry_remove(uint16_t addr) {
    LOCK();
    int idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_infos[i].address == addr) { idx = i; break; }
    }
    if (idx < 0) { UNLOCK(); return false; }

    // Shift down
    for (int i = idx; i < s_count - 1; i++) {
        s_infos[i] = s_infos[i + 1];
        s_data[i]  = s_data[i + 1];
    }
    s_count--;
    save_json();
    UNLOCK();
    return true;
}

bool registry_update(uint16_t addr, const char *name,
                     int min_dist, int max_dist, float capacity) {
    LOCK();
    int idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_infos[i].address == addr) { idx = i; break; }
    }
    if (idx < 0) { UNLOCK(); return false; }

    if (name)        sanitize_name(s_infos[idx].name, name, TX_NAME_MAX);
    if (min_dist > 0) s_infos[idx].min_dist_cm     = min_dist;
    if (max_dist > 0) s_infos[idx].max_dist_cm     = max_dist;
    if (capacity > 0) s_infos[idx].capacity_liters = capacity;

    if (s_data[idx].data_valid) calc_water(idx);
    save_json();
    UNLOCK();
    return true;
}

bool registry_set_enabled(uint16_t addr, bool enabled) {
    LOCK();
    for (int i = 0; i < s_count; i++) {
        if (s_infos[i].address == addr) {
            s_infos[i].enabled = enabled;
            save_json();
            UNLOCK();
            return true;
        }
    }
    UNLOCK();
    return false;
}

bool registry_update_data(uint16_t addr, int raw_dist, int bat_pct,
                           float bat_v, uint32_t msg_id, int rssi, int snr,
                           const char *fw_version) {
    LOCK();
    int idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_infos[i].address == addr && s_infos[i].enabled) { idx = i; break; }
    }
    if (idx < 0) {
        ESP_LOGW(TAG, "Unknown TX addr=%d (not registered)", addr);
        UNLOCK();
        return false;
    }

    s_data[idx].raw_dist_cm     = raw_dist;
    s_data[idx].battery_pct     = bat_pct;
    s_data[idx].battery_voltage = bat_v;
    s_data[idx].last_msg_id     = msg_id;
    s_data[idx].rssi            = rssi;
    s_data[idx].snr             = snr;
    s_data[idx].last_update_us  = esp_timer_get_time();
    s_data[idx].packets_rx++;
    s_data[idx].data_valid      = true;
    s_data[idx].state           = TX_STATE_CONNECTED;
    bool ver_changed = false;
    if (fw_version && fw_version[0]) {
        if (strcmp(s_data[idx].fw_version, fw_version) != 0) {
            ver_changed = true;
        }
        strncpy(s_data[idx].fw_version, fw_version, sizeof(s_data[idx].fw_version) - 1);
        s_data[idx].fw_version[sizeof(s_data[idx].fw_version) - 1] = '\0';
    }

    calc_water(idx);
    // Persist when firmware version is first seen or changes (e.g. after OTA)
    if (ver_changed) save_json();
    UNLOCK();

    ESP_LOGI(TAG, "TX[%d] '%s': %d%% (%.0fL) bat=%d%% rssi=%d",
             addr, s_infos[idx].name,
             s_data[idx].water_pct, s_data[idx].water_liters,
             bat_pct, rssi);
    return true;
}

int registry_count(void) {
    LOCK(); int c = s_count; UNLOCK(); return c;
}

int registry_online_count(void) {
    LOCK();
    int c = 0;
    for (int i = 0; i < s_count; i++) {
        if (s_infos[i].enabled && s_data[i].state == TX_STATE_CONNECTED) c++;
    }
    UNLOCK();
    return c;
}

int registry_find(uint16_t addr) {
    LOCK();
    for (int i = 0; i < s_count; i++) {
        if (s_infos[i].address == addr) { UNLOCK(); return i; }
    }
    UNLOCK();
    return -1;
}

bool registry_get_info(int idx, tx_info_t *out) {
    LOCK();
    if (idx < 0 || idx >= s_count) { UNLOCK(); return false; }
    *out = s_infos[idx];
    UNLOCK();
    return true;
}

bool registry_get_data(int idx, tx_data_t *out) {
    LOCK();
    if (idx < 0 || idx >= s_count) { UNLOCK(); return false; }
    *out = s_data[idx];
    UNLOCK();
    return true;
}

tx_state_t registry_worst_state(void) {
    LOCK();
    tx_state_t worst = TX_STATE_CONNECTED;
    bool any = false;
    for (int i = 0; i < s_count; i++) {
        if (!s_infos[i].enabled) continue;
        any = true;
        if (s_data[i].state > worst) worst = s_data[i].state;
    }
    UNLOCK();
    return any ? worst : TX_STATE_WAITING;
}

void registry_update_states(void) {
    int64_t now = esp_timer_get_time();
    LOCK();
    for (int i = 0; i < s_count; i++) {
        if (!s_data[i].data_valid) { s_data[i].state = TX_STATE_WAITING; continue; }
        int64_t age = now - s_data[i].last_update_us;
        if      (age > LOST_US)  s_data[i].state = TX_STATE_LOST;
        else if (age > STALE_US) s_data[i].state = TX_STATE_STALE;
        else                     s_data[i].state = TX_STATE_CONNECTED;
    }
    UNLOCK();
}

void registry_sanitize_name(const char *name, char *out, size_t out_len) {
    size_t j = 0;
    for (size_t i = 0; name[i] && j < out_len - 1; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') out[j++] = c + 32;
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[j++] = c;
        else if (c == ' ' || c == '-') out[j++] = '_';
    }
    // Trim trailing underscores
    while (j > 0 && out[j-1] == '_') j--;
    out[j] = '\0';
}

const char *registry_state_str(tx_state_t state) {
    switch (state) {
        case TX_STATE_CONNECTED: return "online";
        case TX_STATE_STALE:     return "stale";
        case TX_STATE_LOST:      return "offline";
        default:                 return "waiting";
    }
}

void registry_clear_all(void) {
    LOCK();
    s_count = 0;
    memset(s_infos, 0, sizeof(s_infos));
    memset(s_data,  0, sizeof(s_data));
    save_json();
    UNLOCK();
    ESP_LOGI(TAG, "Registry cleared");
}

bool registry_set_remote_config(uint16_t addr, uint32_t sleep_s, uint8_t samples) {
    LOCK();
    int idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_infos[i].address == addr) { idx = i; break; }
    }
    if (idx < 0) { UNLOCK(); return false; }

    s_infos[idx].sleep_s = sleep_s;
    s_infos[idx].samples = samples;
    s_infos[idx].pending_config = true;
    save_json();
    UNLOCK();
    ESP_LOGI(TAG, "Remote config queued for %d: sleep=%us samp=%u", addr, (unsigned)sleep_s, samples);
    return true;
}

bool registry_get_pending_config(uint16_t addr, uint32_t *sleep_out, uint8_t *samples_out) {
    LOCK();
    int idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_infos[i].address == addr) { idx = i; break; }
    }
    if (idx >= 0 && s_infos[idx].pending_config) {
        if (sleep_out)   *sleep_out   = s_infos[idx].sleep_s;
        if (samples_out) *samples_out = s_infos[idx].samples;
        // DO NOT clear here — cleared only on SET_ACK receipt
        // so config retries automatically on next TX wake if delivery fails
        UNLOCK();
        return true;
    }
    UNLOCK();
    return false;
}

bool registry_clear_pending_config(uint16_t addr) {
    LOCK();
    for (int i = 0; i < s_count; i++) {
        if (s_infos[i].address == addr && s_infos[i].pending_config) {
            s_infos[i].pending_config = false;
            save_json();
            UNLOCK();
            ESP_LOGI(TAG, "Config confirmed for addr %d — pending cleared", addr);
            return true;
        }
    }
    UNLOCK();
    return false;
}

void registry_set_all_ota_pending(bool pending) {
    LOCK();
    for (int i = 0; i < s_count; i++) {
        s_infos[i].ota_pending = pending;
        s_infos[i].ota_offset = 0;
    }
    save_json();
    UNLOCK();
}

bool registry_get_ota_status(uint16_t addr, bool *pending_out, uint32_t *offset_out) {
    LOCK();
    int idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_infos[i].address == addr) { idx = i; break; }
    }
    if (idx >= 0) {
        if (pending_out) *pending_out = s_infos[idx].ota_pending;
        if (offset_out)  *offset_out  = s_infos[idx].ota_offset;
        UNLOCK();
        return true;
    }
    UNLOCK();
    return false;
}

void registry_set_ota_progress(uint16_t addr, uint32_t offset, bool pending) {
    LOCK();
    int idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_infos[i].address == addr) { idx = i; break; }
    }
    if (idx >= 0) {
        s_infos[idx].ota_offset = offset;
        s_infos[idx].ota_pending = pending;
        // Only persist on completion/abort — caller uses registry_persist()
        // for periodic checkpoints during transfer
        if (!pending) save_json();
    }
    UNLOCK();
}

void registry_persist(void) {
    LOCK();
    save_json();
    UNLOCK();
}

void registry_get_spiffs_info(size_t *total, size_t *used) {
    esp_spiffs_info(NULL, total, used);
}
