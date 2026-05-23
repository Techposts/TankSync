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
static const char *HIST_PATH = "/spiffs/tx_hist.bin";

// Data-stale thresholds (match config.h)
// Adaptive stale/lost thresholds scale per-tank with each TX's configured sleep_s.
// stale = 2 missed wakes + 60s jitter; lost = 4 missed wakes + 60s jitter.
// Fallback constants used only when sleep_s is uninitialized (legacy / pre-config).
#define STALE_US_FALLBACK   (600LL * 1000000LL)     // 10 min
#define LOST_US_FALLBACK    (900LL * 1000000LL)     // 15 min

// ── History storage ──────────────────────────────────────────────────────────
// Ring buffer indexed by (epoch_30min) % TX_HIST_SLOTS. We track head_30min
// (the bucket number, not modulo'd) so reads can compute the absolute time of
// every slot. Per-slot bytes are 0xFF when empty (no LoRa packet that bucket).
typedef struct {
    uint8_t  pct[TX_HIST_SLOTS];
    uint8_t  volt_dv[TX_HIST_SLOTS];
    uint32_t head_30min;     // epoch_30min of the most-recent written slot
    bool     valid;          // true once at least one slot has been written
} tank_hist_t;

#define HIST_FILE_MAGIC   0x54534948u  // 'H''I''S''T' little-endian
#define HIST_FILE_VERSION 1

// ── Internal storage ──────────────────────────────────────────────────────────
static tx_info_t        s_infos[MAX_TRANSMITTERS];
static tx_data_t        s_data [MAX_TRANSMITTERS];

// ── Sensor variance detection (rx-v2.8.3+) ──────────────────────────────────
// Per-TX ring buffer of recent raw_dist readings. When spread (max-min) over a
// full window is ≤ STUCK_SPREAD_CM, the sensor is reporting a constant value
// regardless of water level — symptom of a defective JSN-SR04M/AJ-SR04M that
// reports its ~20-25 cm min-range as a constant. Bug-A's dist=0 safety net
// doesn't catch this because the value looks plausible. Kept private to this
// module; only the resulting sensor_stuck bool surfaces via tx_data_t.
#define STUCK_WINDOW_SIZE 20
#define STUCK_SPREAD_CM   2
static struct {
    uint16_t buf[STUCK_WINDOW_SIZE];
    uint8_t  idx;
    uint8_t  fill;
} s_dist_history[MAX_TRANSMITTERS];

static void variance_reset(int idx) {
    if (idx < 0 || idx >= MAX_TRANSMITTERS) return;
    s_dist_history[idx].idx = 0;
    s_dist_history[idx].fill = 0;
}

static bool variance_push_and_check(int idx, int raw_dist) {
    if (idx < 0 || idx >= MAX_TRANSMITTERS) return false;
    if (raw_dist <= 0) return false;   // skip dist=0 (sensor failure path)
    uint16_t v = (raw_dist > 65535) ? 65535 : (uint16_t)raw_dist;
    s_dist_history[idx].buf[s_dist_history[idx].idx] = v;
    s_dist_history[idx].idx = (s_dist_history[idx].idx + 1) % STUCK_WINDOW_SIZE;
    if (s_dist_history[idx].fill < STUCK_WINDOW_SIZE) {
        s_dist_history[idx].fill++;
        return false;   // not enough data yet — don't flag
    }
    uint16_t mn = 0xFFFF, mx = 0;
    for (uint8_t i = 0; i < STUCK_WINDOW_SIZE; i++) {
        uint16_t b = s_dist_history[idx].buf[i];
        if (b < mn) mn = b;
        if (b > mx) mx = b;
    }
    return (mx - mn) <= STUCK_SPREAD_CM;
}
static tank_hist_t      s_hist [MAX_TRANSMITTERS];
static bool             s_hist_dirty = false;
static int              s_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

// Tombstone archive — entries removed via registry_remove() land here so a
// returning TX (same MAC) can be resurrected with all user config intact.
// FIFO eviction when full (oldest tombstone evicted to make room for the new
// one). History is NOT preserved in the archive — only tx_info_t metadata.
#define MAX_TOMBSTONES 10
static tx_info_t        s_tombstones[MAX_TOMBSTONES];
static int              s_tombstone_count = 0;

// Membership-change callback. Set by mqtt_manager at init so registry
// mutations trigger a fresh retained-manifest publish, which the cloud
// reconciler uses as the source of truth.
static registry_change_cb_t s_change_cb = NULL;

void registry_set_change_callback(registry_change_cb_t cb) {
    s_change_cb = cb;
}

// Wrapper to fire the callback OUTSIDE the registry lock — calling back into
// MQTT publish while holding our mutex risks deadlock if mqtt_manager ever
// needs to read the registry (which mqtt_publish_registry does). Always invoke
// after UNLOCK and after save_json().
static inline void notify_change(void) {
    if (s_change_cb) s_change_cb();
}

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
        // Persist MAC as 12-char lowercase hex. Zero-MAC (legacy entry) is omitted.
        const uint8_t *m = s_infos[i].mac;
        if (m[0] || m[1] || m[2] || m[3] || m[4] || m[5]) {
            char machex[13];
            snprintf(machex, sizeof(machex), "%02x%02x%02x%02x%02x%02x",
                     m[0], m[1], m[2], m[3], m[4], m[5]);
            cJSON_AddStringToObject(tx, "mac", machex);
        }
        cJSON_AddStringToObject(tx, "name",     s_infos[i].name);
        cJSON_AddNumberToObject(tx, "min_dist", s_infos[i].min_dist_cm);
        cJSON_AddNumberToObject(tx, "max_dist", s_infos[i].max_dist_cm);
        cJSON_AddNumberToObject(tx, "capacity", s_infos[i].capacity_liters);
        cJSON_AddBoolToObject  (tx, "enabled",  s_infos[i].enabled);
        cJSON_AddNumberToObject(tx, "sleep",    s_infos[i].sleep_s);
        cJSON_AddNumberToObject(tx, "samples",  s_infos[i].samples);
        cJSON_AddNumberToObject(tx, "lora_pwr", s_infos[i].lora_pwr);
        cJSON_AddBoolToObject  (tx, "pending",  s_infos[i].pending_config);
        cJSON_AddBoolToObject  (tx, "ota_p",    s_infos[i].ota_pending);
        cJSON_AddNumberToObject(tx, "ota_o",    s_infos[i].ota_offset);
        cJSON_AddNumberToObject(tx, "led_col", s_infos[i].led_color_idx);
        // Persist last-known firmware version (survives receiver reboot)
        if (s_data[i].fw_version[0])
            cJSON_AddStringToObject(tx, "fw_ver", s_data[i].fw_version);
        cJSON_AddItemToArray(arr, tx);
    }

    // ── Tombstone archive ──
    // Persisted separately so a returning TX can be resurrected even after
    // a hub reboot. Schema mirrors transmitters[] but only the metadata the
    // user might want restored (history is NOT archived).
    if (s_tombstone_count > 0) {
        cJSON *tombs = cJSON_AddArrayToObject(root, "tombstones");
        for (int i = 0; i < s_tombstone_count; i++) {
            cJSON *tx = cJSON_CreateObject();
            cJSON_AddNumberToObject(tx, "addr",     s_tombstones[i].address);
            const uint8_t *m = s_tombstones[i].mac;
            char machex[13];
            snprintf(machex, sizeof(machex), "%02x%02x%02x%02x%02x%02x",
                     m[0], m[1], m[2], m[3], m[4], m[5]);
            cJSON_AddStringToObject(tx, "mac",      machex);
            cJSON_AddStringToObject(tx, "name",     s_tombstones[i].name);
            cJSON_AddNumberToObject(tx, "min_dist", s_tombstones[i].min_dist_cm);
            cJSON_AddNumberToObject(tx, "max_dist", s_tombstones[i].max_dist_cm);
            cJSON_AddNumberToObject(tx, "capacity", s_tombstones[i].capacity_liters);
            cJSON_AddNumberToObject(tx, "sleep",    s_tombstones[i].sleep_s);
            cJSON_AddNumberToObject(tx, "samples",  s_tombstones[i].samples);
            cJSON_AddNumberToObject(tx, "lora_pwr", s_tombstones[i].lora_pwr);
            cJSON_AddNumberToObject(tx, "led_col",  s_tombstones[i].led_color_idx);
            cJSON_AddItemToArray(tombs, tx);
        }
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
        // Parse MAC if present. Legacy entries persisted before the MAC field
        // existed simply omit it — `info->mac` stays all-zeros which the
        // PAIR_REQ handler treats as "unknown identity" (no MAC match).
        memset(info->mac, 0, 6);
        const char *machex = cJSON_GetStringValue(cJSON_GetObjectItem(tx, "mac"));
        if (machex && strlen(machex) == 12) {
            for (int b = 0; b < 6; b++) {
                unsigned int byte_val = 0;
                if (sscanf(machex + b * 2, "%2x", &byte_val) == 1) {
                    info->mac[b] = (uint8_t)byte_val;
                }
            }
        }
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
        cJSON *lpw = cJSON_GetObjectItem(tx, "lora_pwr");
        info->lora_pwr = lpw ? (uint8_t)cJSON_GetNumberValue(lpw) : 0;  // 0 = leave at TX default
        cJSON *pe = cJSON_GetObjectItem(tx, "pending");
        info->pending_config = pe ? cJSON_IsTrue(pe) : false;
        cJSON *op = cJSON_GetObjectItem(tx, "ota_p");
        info->ota_pending = op ? cJSON_IsTrue(op) : false;
        cJSON *oo = cJSON_GetObjectItem(tx, "ota_o");
        info->ota_offset = oo ? (uint32_t)cJSON_GetNumberValue(oo) : 0;
        cJSON *lc = cJSON_GetObjectItem(tx, "led_col");
        info->led_color_idx = lc ? (int8_t)cJSON_GetNumberValue(lc) : -1;

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

    // ── Tombstone archive ──
    // Restore on boot so a soft-deleted entry can be resurrected even after
    // the hub reboots between delete and re-pair. Older JSON (rx <2.7.11)
    // simply omits this field — load proceeds with empty archive.
    s_tombstone_count = 0;
    cJSON *tombs = cJSON_GetObjectItem(root, "tombstones");
    if (tombs && cJSON_IsArray(tombs)) {
        cJSON *t;
        cJSON_ArrayForEach(t, tombs) {
            if (s_tombstone_count >= MAX_TOMBSTONES) break;
            tx_info_t *info = &s_tombstones[s_tombstone_count];
            memset(info, 0, sizeof(tx_info_t));
            info->address = (uint16_t)cJSON_GetNumberValue(cJSON_GetObjectItem(t, "addr"));
            const char *machex = cJSON_GetStringValue(cJSON_GetObjectItem(t, "mac"));
            if (machex && strlen(machex) == 12) {
                for (int b = 0; b < 6; b++) {
                    unsigned int byte_val = 0;
                    if (sscanf(machex + b * 2, "%2x", &byte_val) == 1) info->mac[b] = (uint8_t)byte_val;
                }
            }
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(t, "name"));
            sanitize_name(info->name, name ? name : "Tank", TX_NAME_MAX);
            info->min_dist_cm     = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(t, "min_dist"));
            info->max_dist_cm     = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(t, "max_dist"));
            info->capacity_liters = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(t, "capacity"));
            info->sleep_s         = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(t, "sleep"));
            info->samples         = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(t, "samples"));
            info->lora_pwr        = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(t, "lora_pwr"));
            cJSON *lc = cJSON_GetObjectItem(t, "led_col");
            info->led_color_idx   = lc ? (int8_t)cJSON_GetNumberValue(lc) : -1;
            info->enabled         = true;  // archived entries are enabled by default on restore
            s_tombstone_count++;
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d transmitter(s), %d tombstone(s)", s_count, s_tombstone_count);
    return s_count > 0;
}

// ── History persistence (binary blob, /spiffs/tx_hist.bin) ───────────────────
// Layout (all little-endian):
//   [0..3]   magic = HIST_FILE_MAGIC
//   [4]      version = HIST_FILE_VERSION
//   [5]      num_records (1..MAX_TRANSMITTERS)
//   [6..7]   reserved (zero)
//   [8 + i*(2+4+1+96+1)] per record:
//     [+0..1]   addr (uint16)
//     [+2..5]   head_30min (uint32)
//     [+6]      valid flag (uint8)
//     [+7..54]  pct[48]
//     [+55..102] volt_dv[48]
// Total file size at 10 tanks ≈ 1038 bytes. Trivial.
#define HIST_REC_SIZE  (2 + 4 + 1 + TX_HIST_SLOTS + TX_HIST_SLOTS)
#define HIST_HDR_SIZE  8

static void hist_save(void) {
    FILE *f = fopen(HIST_PATH, "wb");
    if (!f) { ESP_LOGW(TAG, "hist: cannot open %s for write", HIST_PATH); return; }

    uint8_t hdr[HIST_HDR_SIZE] = {0};
    uint32_t magic = HIST_FILE_MAGIC;
    memcpy(hdr, &magic, 4);
    hdr[4] = HIST_FILE_VERSION;
    hdr[5] = (uint8_t)s_count;
    fwrite(hdr, 1, HIST_HDR_SIZE, f);

    for (int i = 0; i < s_count; i++) {
        uint8_t rec[HIST_REC_SIZE];
        uint16_t addr = s_infos[i].address;
        uint32_t head = s_hist[i].head_30min;
        memcpy(rec + 0, &addr, 2);
        memcpy(rec + 2, &head, 4);
        rec[6] = s_hist[i].valid ? 1 : 0;
        memcpy(rec + 7,                    s_hist[i].pct,     TX_HIST_SLOTS);
        memcpy(rec + 7 + TX_HIST_SLOTS,    s_hist[i].volt_dv, TX_HIST_SLOTS);
        fwrite(rec, 1, HIST_REC_SIZE, f);
    }
    fclose(f);
    s_hist_dirty = false;
    ESP_LOGI(TAG, "hist: saved %d record(s)", s_count);
}

static void hist_load(void) {
    // Initialise all buffers to "empty" first — load_json may have populated
    // s_count already, so we size our zero-fill to s_count.
    for (int i = 0; i < MAX_TRANSMITTERS; i++) {
        memset(s_hist[i].pct,     TX_HIST_PCT_EMPTY,  TX_HIST_SLOTS);
        memset(s_hist[i].volt_dv, TX_HIST_VOLT_EMPTY, TX_HIST_SLOTS);
        s_hist[i].head_30min = 0;
        s_hist[i].valid      = false;
    }

    FILE *f = fopen(HIST_PATH, "rb");
    if (!f) { ESP_LOGI(TAG, "hist: no existing %s — starting fresh", HIST_PATH); return; }

    uint8_t hdr[HIST_HDR_SIZE];
    if (fread(hdr, 1, HIST_HDR_SIZE, f) != HIST_HDR_SIZE) { fclose(f); return; }
    uint32_t magic; memcpy(&magic, hdr, 4);
    if (magic != HIST_FILE_MAGIC || hdr[4] != HIST_FILE_VERSION) {
        ESP_LOGW(TAG, "hist: magic/version mismatch — discarding");
        fclose(f);
        return;
    }
    int n = hdr[5];
    for (int r = 0; r < n; r++) {
        uint8_t rec[HIST_REC_SIZE];
        if (fread(rec, 1, HIST_REC_SIZE, f) != HIST_REC_SIZE) break;
        uint16_t addr; memcpy(&addr, rec, 2);
        // Find the matching slot in the (already-loaded) registry. If the tank
        // was deleted while powered down, we silently drop its history.
        int idx = -1;
        for (int i = 0; i < s_count; i++) {
            if (s_infos[i].address == addr) { idx = i; break; }
        }
        if (idx < 0) continue;
        memcpy(&s_hist[idx].head_30min, rec + 2, 4);
        s_hist[idx].valid = (rec[6] != 0);
        memcpy(s_hist[idx].pct,     rec + 7,                 TX_HIST_SLOTS);
        memcpy(s_hist[idx].volt_dv, rec + 7 + TX_HIST_SLOTS, TX_HIST_SLOTS);
    }
    fclose(f);
    ESP_LOGI(TAG, "hist: loaded %d record(s)", n);
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
    hist_load(); // Restore 24-h rolling history per tank (tx_hist.bin)
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
    memset(s_infos[idx].mac, 0, 6);       // PAIR_REQ handler fills this via registry_set_mac()
    s_infos[idx].min_dist_cm     = min_dist > 0 ? min_dist : 30;
    s_infos[idx].max_dist_cm     = max_dist > 0 ? max_dist : 120;
    s_infos[idx].capacity_liters = capacity > 0 ? capacity : 942.5f;
    s_infos[idx].enabled         = true;
    s_infos[idx].sleep_s         = 300;
    s_infos[idx].samples         = 5;
    s_infos[idx].lora_pwr        = 0;     // 0 = leave at TX default (no override)
    s_infos[idx].pending_config  = false;
    s_infos[idx].ota_pending     = false;
    s_infos[idx].ota_offset      = 0;
    sanitize_name(s_infos[idx].name, name ? name : "Tank", TX_NAME_MAX);

    s_data[idx].address = addr;
    s_data[idx].state   = TX_STATE_WAITING;
    variance_reset(idx);   // fresh slot starts with empty variance window
    s_count++;

    save_json();
    UNLOCK();
    notify_change();
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

    // ── Tombstone the entry before shift-out ──
    // Preserves name + address + capacity + alerts + sleep_s + lora_pwr + MAC
    // so a returning TX with the same MAC can be resurrected without losing
    // any user config. History is NOT preserved (would need its own archive).
    // Only entries with a known MAC are worth archiving — legacy zero-MAC
    // entries can never be matched on re-pair, so archiving them just wastes
    // a tombstone slot.
    const uint8_t *m = s_infos[idx].mac;
    bool has_mac = false;
    for (int b = 0; b < 6; b++) if (m[b]) { has_mac = true; break; }

    if (has_mac) {
        // FIFO eviction if archive full — oldest tombstone is at index 0.
        if (s_tombstone_count >= MAX_TOMBSTONES) {
            for (int i = 0; i < MAX_TOMBSTONES - 1; i++) {
                s_tombstones[i] = s_tombstones[i + 1];
            }
            s_tombstone_count--;
        }
        s_tombstones[s_tombstone_count] = s_infos[idx];
        s_tombstone_count++;
        ESP_LOGI(TAG, "Tombstoned addr=%d '%s' (MAC %02x:%02x:%02x:%02x:%02x:%02x) — %d in archive",
                 addr, s_infos[idx].name,
                 m[0], m[1], m[2], m[3], m[4], m[5], s_tombstone_count);
    }

    // Shift down — history rides along so each tank keeps its own buffer
    for (int i = idx; i < s_count - 1; i++) {
        s_infos[i] = s_infos[i + 1];
        s_data[i]  = s_data[i + 1];
        s_hist[i]  = s_hist[i + 1];
        s_dist_history[i] = s_dist_history[i + 1];
    }
    // Clear the now-vacated tail slot
    memset(s_hist[s_count - 1].pct,     TX_HIST_PCT_EMPTY,  TX_HIST_SLOTS);
    memset(s_hist[s_count - 1].volt_dv, TX_HIST_VOLT_EMPTY, TX_HIST_SLOTS);
    s_hist[s_count - 1].head_30min = 0;
    s_hist[s_count - 1].valid      = false;
    variance_reset(s_count - 1);
    s_count--;
    s_hist_dirty = true;
    save_json();
    UNLOCK();
    notify_change();
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

bool registry_set_led_color(uint16_t addr, int8_t color_idx) {
    LOCK();
    for (int i = 0; i < s_count; i++) {
        if (s_infos[i].address == addr) {
            s_infos[i].led_color_idx = color_idx;
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
                           const char *fw_version,
                           char power_mode, int32_t current_ma, int32_t power_mw,
                           char sensor_status) {
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

    // Sensor-failure handling. TX sends raw_dist=0 when its ultrasonic
    // read errored out (timeout, no echo, out-of-range, etc.). Without
    // this guard, calc_water(0) computes level = max_dist - 0 = full
    // range -> water_pct = 100% -- a silently dangerous lie: a customer
    // with a near-empty tank would see "100% full" and not refill.
    //
    // On sensor failure we keep last-known raw_dist_cm / water_pct /
    // water_liters (preserves the prior good reading) but DO update
    // everything else so the TX still appears alive in the UI and
    // battery/firmware/RSSI tracking continues. A `sensor_error` flag
    // tells the UI to badge this tank so the user investigates rather
    // than trusting the (now stale) reading.
    // Two paths: explicit signal from TX v2.0.12+ ('o'/'e'), or fall back to
    // the dist==0 heuristic for older TX firmware that doesn't send sensor_status.
    bool sensor_failed;
    if (sensor_status == 'o') {
        sensor_failed = false;          // TX explicitly says sensor is OK
    } else if (sensor_status == 'e') {
        sensor_failed = true;           // TX explicitly says sensor failed
    } else {
        sensor_failed = (raw_dist == 0); // Old TX (sensor_status='u'): heuristic
    }
    if (!sensor_failed) {
        s_data[idx].raw_dist_cm = raw_dist;
        // Push only known-good readings into the variance window. A dist=0
        // failure path would corrupt the spread calculation; sensor_failed
        // already alerts via the sensor_error chip so we don't need both.
        s_data[idx].sensor_stuck = variance_push_and_check(idx, raw_dist);
    } else {
        // Sensor failed this packet — leave sensor_stuck as-is (sticky). The
        // chip clears on the next good reading whose spread exceeds threshold.
    }
    s_data[idx].sensor_error    = sensor_failed;
    s_data[idx].battery_pct     = bat_pct;
    s_data[idx].battery_voltage = bat_v;
    s_data[idx].last_msg_id     = msg_id;
    s_data[idx].rssi            = rssi;
    s_data[idx].snr             = snr;
    s_data[idx].last_update_us  = esp_timer_get_time();
    s_data[idx].packets_rx++;
    s_data[idx].data_valid      = true;
    s_data[idx].state           = TX_STATE_CONNECTED;
    s_data[idx].power_mode      = (power_mode == 'v' || power_mode == 'i' ||
                                   power_mode == 'n') ? power_mode : '?';
    s_data[idx].current_ma      = current_ma;
    s_data[idx].power_mw        = power_mw;
    s_data[idx].charging        = (s_data[idx].power_mode == 'i' && current_ma < 0);
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

int registry_find_by_mac(const uint8_t mac[6]) {
    // All-zero MAC means "unknown identity" — never match legacy entries
    // that lack a MAC against a TX that hasn't supplied one either.
    if (!mac) return -1;
    bool any_set = false;
    for (int b = 0; b < 6; b++) if (mac[b]) { any_set = true; break; }
    if (!any_set) return -1;

    LOCK();
    for (int i = 0; i < s_count; i++) {
        if (memcmp(s_infos[i].mac, mac, 6) == 0) { UNLOCK(); return i; }
    }
    UNLOCK();
    return -1;
}

uint16_t registry_alloc_small_addr(void) {
    LOCK();
    // Lowest free integer in [1, 99]. Linear scan — fine for MAX_TRANSMITTERS=10.
    for (uint16_t candidate = 1; candidate <= 99; candidate++) {
        bool used = false;
        for (int i = 0; i < s_count; i++) {
            if (s_infos[i].address == candidate) { used = true; break; }
        }
        if (!used) { UNLOCK(); return candidate; }
    }
    UNLOCK();
    return 0;  // pool exhausted (effectively unreachable at MAX_TRANSMITTERS=10)
}

bool registry_set_mac(uint16_t addr, const uint8_t mac[6]) {
    if (!mac) return false;
    LOCK();
    for (int i = 0; i < s_count; i++) {
        if (s_infos[i].address == addr) {
            memcpy(s_infos[i].mac, mac, 6);
            UNLOCK();
            save_json();  // persist immediately; MAC is identity, never recompute
            return true;
        }
    }
    UNLOCK();
    return false;
}

int registry_archive_find_by_mac(const uint8_t mac[6]) {
    if (!mac) return -1;
    bool any_set = false;
    for (int b = 0; b < 6; b++) if (mac[b]) { any_set = true; break; }
    if (!any_set) return -1;  // all-zero sentinel never matches anything

    LOCK();
    for (int i = 0; i < s_tombstone_count; i++) {
        if (memcmp(s_tombstones[i].mac, mac, 6) == 0) { UNLOCK(); return i; }
    }
    UNLOCK();
    return -1;
}

bool registry_archive_restore(int tombstone_idx) {
    LOCK();
    if (tombstone_idx < 0 || tombstone_idx >= s_tombstone_count) {
        UNLOCK(); return false;
    }
    if (s_count >= MAX_TRANSMITTERS) {
        UNLOCK();
        ESP_LOGW(TAG, "Cannot restore tombstone — live registry full");
        return false;
    }
    // If something else has claimed the archived address while it was
    // tombstoned, refuse to restore — caller should allocate a new address
    // instead of forcing a collision.
    uint16_t archived_addr = s_tombstones[tombstone_idx].address;
    for (int i = 0; i < s_count; i++) {
        if (s_infos[i].address == archived_addr) {
            UNLOCK();
            ESP_LOGW(TAG, "Cannot restore tombstone — address %d taken by live entry", archived_addr);
            return false;
        }
    }

    // Copy tombstone into live registry tail
    int new_idx = s_count;
    s_infos[new_idx] = s_tombstones[tombstone_idx];
    memset(&s_data[new_idx], 0, sizeof(tx_data_t));
    s_data[new_idx].address = archived_addr;
    s_data[new_idx].state   = TX_STATE_WAITING;
    // Fresh history slot — old history wasn't archived
    memset(s_hist[new_idx].pct,     TX_HIST_PCT_EMPTY,  TX_HIST_SLOTS);
    memset(s_hist[new_idx].volt_dv, TX_HIST_VOLT_EMPTY, TX_HIST_SLOTS);
    s_hist[new_idx].head_30min = 0;
    s_hist[new_idx].valid      = false;
    s_count++;

    // Remove tombstone (shift remaining down)
    for (int i = tombstone_idx; i < s_tombstone_count - 1; i++) {
        s_tombstones[i] = s_tombstones[i + 1];
    }
    memset(&s_tombstones[s_tombstone_count - 1], 0, sizeof(tx_info_t));
    s_tombstone_count--;

    save_json();
    UNLOCK();
    notify_change();
    ESP_LOGI(TAG, "Restored archived entry addr=%d '%s' (%d tombstones left)",
             archived_addr, s_infos[new_idx].name, s_tombstone_count);
    return true;
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
        uint32_t sleep_s = s_infos[i].sleep_s;
        int64_t stale_us = sleep_s > 0
            ? ((int64_t)sleep_s * 2 + 60) * 1000000LL
            : STALE_US_FALLBACK;
        int64_t lost_us = sleep_s > 0
            ? ((int64_t)sleep_s * 4 + 60) * 1000000LL
            : LOST_US_FALLBACK;
        if      (age > lost_us)  s_data[i].state = TX_STATE_LOST;
        else if (age > stale_us) s_data[i].state = TX_STATE_STALE;
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
    memset(s_dist_history, 0, sizeof(s_dist_history));
    for (int i = 0; i < MAX_TRANSMITTERS; i++) {
        memset(s_hist[i].pct,     TX_HIST_PCT_EMPTY,  TX_HIST_SLOTS);
        memset(s_hist[i].volt_dv, TX_HIST_VOLT_EMPTY, TX_HIST_SLOTS);
        s_hist[i].head_30min = 0;
        s_hist[i].valid      = false;
    }
    s_hist_dirty = true;
    save_json();
    hist_save();
    UNLOCK();
    notify_change();
    ESP_LOGI(TAG, "Registry cleared");
}

bool registry_set_remote_config(uint16_t addr, uint32_t sleep_s, uint8_t samples, uint8_t pwr) {
    LOCK();
    int idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_infos[i].address == addr) { idx = i; break; }
    }
    if (idx < 0) { UNLOCK(); return false; }

    s_infos[idx].sleep_s = sleep_s;
    s_infos[idx].samples = samples;
    s_infos[idx].lora_pwr = pwr;     // 0 = leave at TX default
    s_infos[idx].pending_config = true;
    save_json();
    UNLOCK();
    ESP_LOGI(TAG, "Remote config queued for %d: sleep=%us samp=%u pwr=%u",
             addr, (unsigned)sleep_s, samples, pwr);
    return true;
}

bool registry_get_pending_config(uint16_t addr, uint32_t *sleep_out, uint8_t *samples_out, uint8_t *pwr_out) {
    LOCK();
    int idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_infos[i].address == addr) { idx = i; break; }
    }
    if (idx >= 0 && s_infos[idx].pending_config) {
        if (sleep_out)   *sleep_out   = s_infos[idx].sleep_s;
        if (samples_out) *samples_out = s_infos[idx].samples;
        if (pwr_out)     *pwr_out     = s_infos[idx].lora_pwr;
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

// ── History — slot capture, persist, accessor ────────────────────────────────

bool registry_history_tick(int64_t now) {
    if (now <= 0) return false;  // pre-NTP — skip until wall clock is known

    uint32_t cur_30min = (uint32_t)(now / TX_HIST_SLOT_SECS);
    bool any_written = false;

    LOCK();
    for (int i = 0; i < s_count; i++) {
        if (!s_infos[i].enabled)        continue;
        if (!s_data[i].data_valid)      continue;
        if (s_data[i].state != TX_STATE_CONNECTED) continue;  // only fresh data

        tank_hist_t *h = &s_hist[i];
        if (h->valid && h->head_30min == cur_30min) continue; // already wrote this bucket

        // If there's a multi-bucket gap, clear the slots we skipped over so
        // they show up as "no data" not "old wrong data". When the gap is
        // larger than the buffer (>= 24h offline), every slot is invalidated.
        if (h->valid) {
            uint32_t gap = cur_30min - h->head_30min;
            if (gap >= TX_HIST_SLOTS) {
                memset(h->pct,     TX_HIST_PCT_EMPTY,  TX_HIST_SLOTS);
                memset(h->volt_dv, TX_HIST_VOLT_EMPTY, TX_HIST_SLOTS);
            } else {
                for (uint32_t g = 1; g < gap; g++) {
                    int slot = (int)((h->head_30min + g) % TX_HIST_SLOTS);
                    h->pct[slot]     = TX_HIST_PCT_EMPTY;
                    h->volt_dv[slot] = TX_HIST_VOLT_EMPTY;
                }
            }
        }

        int slot = (int)(cur_30min % TX_HIST_SLOTS);
        int pct = s_data[i].water_pct;
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        h->pct[slot] = (uint8_t)pct;

        // Encode voltage as deci-volts × 2 (i.e. 0.05 V steps, 0..12.7 V).
        // 0xFF reserved as "no voltage data" sentinel; clamp top end to 0xFE.
        float v = s_data[i].battery_voltage;
        int dv = (v > 0.0f) ? (int)(v * 20.0f + 0.5f) : -1;
        if (dv < 0) {
            h->volt_dv[slot] = TX_HIST_VOLT_EMPTY;
        } else {
            if (dv > 0xFE) dv = 0xFE;
            h->volt_dv[slot] = (uint8_t)dv;
        }

        h->head_30min = cur_30min;
        h->valid      = true;
        s_hist_dirty  = true;
        any_written   = true;
    }
    UNLOCK();
    return any_written;
}

void registry_history_persist(void) {
    LOCK();
    if (s_hist_dirty) hist_save();
    UNLOCK();
}

bool registry_get_history(int idx, uint8_t out_pct[TX_HIST_SLOTS],
                          uint8_t out_volt_dv[TX_HIST_SLOTS],
                          int64_t *out_head_t) {
    LOCK();
    if (idx < 0 || idx >= s_count || !s_hist[idx].valid) {
        UNLOCK();
        return false;
    }
    // Reorder ring buffer into oldest→newest. The newest slot lives at
    // head_30min % TX_HIST_SLOTS; the oldest is one slot ahead of it (which
    // equals (head + 1) % TX_HIST_SLOTS, i.e. (head - 47) modulo).
    uint32_t head = s_hist[idx].head_30min;
    int head_slot = (int)(head % TX_HIST_SLOTS);
    for (int i = 0; i < TX_HIST_SLOTS; i++) {
        // Walk from oldest (head - 47) to newest (head).
        int rev = TX_HIST_SLOTS - 1 - i;       // 47 .. 0
        int slot = (head_slot - rev + 2 * TX_HIST_SLOTS) % TX_HIST_SLOTS;
        out_pct[i]     = s_hist[idx].pct[slot];
        out_volt_dv[i] = s_hist[idx].volt_dv[slot];
    }
    if (out_head_t) *out_head_t = (int64_t)head * (int64_t)TX_HIST_SLOT_SECS;
    UNLOCK();
    return true;
}

#ifdef DEBUG_HIST_INJECT
bool registry_history_inject(uint16_t addr, const uint8_t pct[TX_HIST_SLOTS],
                             const uint8_t volt_dv[TX_HIST_SLOTS],
                             int64_t head_t) {
    LOCK();
    int idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_infos[i].address == addr) { idx = i; break; }
    }
    if (idx < 0) { UNLOCK(); return false; }
    // The injected window is "oldest..newest" (same shape as the accessor
    // returns), so pct[47] is the newest sample at head_t. Re-encode into the
    // ring layout.
    uint32_t head = (uint32_t)(head_t / TX_HIST_SLOT_SECS);
    int head_slot = (int)(head % TX_HIST_SLOTS);
    for (int i = 0; i < TX_HIST_SLOTS; i++) {
        int rev = TX_HIST_SLOTS - 1 - i;
        int slot = (head_slot - rev + 2 * TX_HIST_SLOTS) % TX_HIST_SLOTS;
        s_hist[idx].pct[slot]     = pct[i];
        s_hist[idx].volt_dv[slot] = volt_dv[i];
    }
    s_hist[idx].head_30min = head;
    s_hist[idx].valid      = true;
    s_hist_dirty           = true;
    UNLOCK();
    return true;
}
#endif
