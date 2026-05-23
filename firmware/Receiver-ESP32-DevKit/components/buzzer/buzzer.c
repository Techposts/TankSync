#include "buzzer.h"

#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "nvs.h"
#include "esp_log.h"

// Pin is passed via buzzer_init(gpio_num) — component stays board-agnostic.

static const char *TAG = "buzzer";
static int s_pin = -1;

// ─── Pattern tables ──────────────────────────────────────────────────────────
// Standard profile only — Quiet replaces with a single short tap; Loud scales
// durations at playback time. See project_buzzer_design_2026_05_23.md tables.

typedef struct {
    uint16_t on_ms;
    uint16_t off_ms;
} buzz_beat_t;

typedef struct {
    const buzz_beat_t *beats;
    uint8_t            beat_count;
    uint8_t            repeat;       // play the whole beat sequence this many times
} buzz_pattern_t;

static const buzz_beat_t BOOT_B[]            = { {80,80},  {160,80},  {240,0} };
static const buzz_beat_t CRITICAL_B[]        = { {200,200},{200,200},{200,0} };
static const buzz_beat_t OVERFLOW_B[]        = { {100,50}, {100,0} };
static const buzz_beat_t SENSOR_OFFLINE_B[]  = { {500,0} };
static const buzz_beat_t TEST_B[]            = { {150,150},{150,0} };
static const buzz_beat_t LOW_WARNING_B[]     = { {150,0} };
static const buzz_beat_t PAIR_SUCCESS_B[]    = { {80,80},  {80,80} };       // repeated 3×
static const buzz_beat_t OTA_SUCCESS_B[]     = { {100,80}, {200,80},  {400,0} };
static const buzz_beat_t OTA_FAILURE_B[]     = { {300,300},{300,300},{300,0} };
static const buzz_beat_t REFILL_DETECTED_B[] = { {100,0} };
static const buzz_beat_t REFILL_COMPLETE_B[] = { {80,80},  {80,0} };
static const buzz_beat_t DRAIN_DETECTED_B[]  = { {200,200},{200,200},{200,200},{200,0} };
static const buzz_beat_t WIFI_RECONNECT_B[]  = { {50,0} };
static const buzz_beat_t TX_LOW_BATTERY_B[]  = { {200,200},{200,0} };

#define PAT(arr, rep) { (arr), (uint8_t)(sizeof(arr)/sizeof(arr[0])), (rep) }

static const buzz_pattern_t STANDARD[BUZZ__COUNT] = {
    [BUZZ_BOOT_COMPLETE]    = PAT(BOOT_B,            1),
    [BUZZ_CRITICAL_LOW]     = PAT(CRITICAL_B,        1),
    [BUZZ_OVERFLOW]         = PAT(OVERFLOW_B,        1),
    [BUZZ_SENSOR_OFFLINE]   = PAT(SENSOR_OFFLINE_B,  1),
    [BUZZ_TEST_BUTTON]      = PAT(TEST_B,            1),
    [BUZZ_LOW_WARNING]      = PAT(LOW_WARNING_B,     1),
    [BUZZ_PAIR_SUCCESS]     = PAT(PAIR_SUCCESS_B,    3),
    [BUZZ_OTA_SUCCESS]      = PAT(OTA_SUCCESS_B,     1),
    [BUZZ_OTA_FAILURE]      = PAT(OTA_FAILURE_B,     1),
    [BUZZ_REFILL_DETECTED]  = PAT(REFILL_DETECTED_B, 1),
    [BUZZ_REFILL_COMPLETE]  = PAT(REFILL_COMPLETE_B, 1),
    [BUZZ_DRAIN_DETECTED]   = PAT(DRAIN_DETECTED_B,  1),
    [BUZZ_WIFI_RECONNECTED] = PAT(WIFI_RECONNECT_B,  1),
    [BUZZ_TX_LOW_BATTERY]   = PAT(TX_LOW_BATTERY_B,  1),
};

static const buzz_beat_t QUIET_TAP_B[] = { {60, 0} };
static const buzz_pattern_t QUIET_PATTERN = PAT(QUIET_TAP_B, 1);

static const char *const EVENT_LABELS[BUZZ__COUNT] = {
    [BUZZ_BOOT_COMPLETE]    = "Boot complete",
    [BUZZ_CRITICAL_LOW]     = "Critical low (<5%)",
    [BUZZ_OVERFLOW]         = "Overflow (>95%)",
    [BUZZ_SENSOR_OFFLINE]   = "Sensor offline (>15min)",
    [BUZZ_TEST_BUTTON]      = "Test button",
    [BUZZ_LOW_WARNING]      = "Low warning (<20%)",
    [BUZZ_PAIR_SUCCESS]     = "Pair success",
    [BUZZ_OTA_SUCCESS]      = "OTA success",
    [BUZZ_OTA_FAILURE]      = "OTA failure",
    [BUZZ_REFILL_DETECTED]  = "Refill detected",
    [BUZZ_REFILL_COMPLETE]  = "Refill complete",
    [BUZZ_DRAIN_DETECTED]   = "Drain detected (leak?)",
    [BUZZ_WIFI_RECONNECTED] = "Wi-Fi reconnected",
    [BUZZ_TX_LOW_BATTERY]   = "TX low battery",
};

// ─── State ───────────────────────────────────────────────────────────────────

typedef struct {
    buzzer_event_t   evt;
    buzzer_profile_t profile_override;   // 0xff = use config; else force this profile
    bool             bypass_gates;       // test button bypasses enable + quiet hours
} buzz_msg_t;

#define BUZZ_PROFILE_USE_CONFIG 0xff

static QueueHandle_t       s_queue        = NULL;
static SemaphoreHandle_t   s_cfg_mutex    = NULL;
static buzzer_config_t     s_cfg;
static bool                s_initialized  = false;

static const char *NVS_NAMESPACE = "buzzer";
static const char *NVS_KEY_BLOB  = "cfg";

// ─── NVS load/save ───────────────────────────────────────────────────────────

void buzzer_default_config(buzzer_config_t *out) {
    memset(out, 0, sizeof(*out));
    out->master_enable = true;
    // Tier 1 (always default on) + Tier 2 (default on, can disable)
    bool tier12_enabled[BUZZ__COUNT] = {
        [BUZZ_BOOT_COMPLETE]    = true,
        [BUZZ_CRITICAL_LOW]     = true,
        [BUZZ_OVERFLOW]         = true,
        [BUZZ_SENSOR_OFFLINE]   = true,
        [BUZZ_TEST_BUTTON]      = true,
        [BUZZ_LOW_WARNING]      = true,
        [BUZZ_PAIR_SUCCESS]     = true,
        [BUZZ_OTA_SUCCESS]      = true,
        [BUZZ_OTA_FAILURE]      = true,
        // Tier 3 — false by default
        [BUZZ_REFILL_DETECTED]  = false,
        [BUZZ_REFILL_COMPLETE]  = false,
        [BUZZ_DRAIN_DETECTED]   = false,
        [BUZZ_WIFI_RECONNECTED] = false,
        [BUZZ_TX_LOW_BATTERY]   = false,
    };
    memcpy(out->alert_enable, tier12_enabled, sizeof(tier12_enabled));
    out->master_profile = BUZZ_PROFILE_STANDARD;
    // Quiet hours: 22:00→07:00 local time (geo_time sets TZ from country code).
    // Critical-low alerts still play during quiet hours unless user opts out.
    out->quiet_start_hour = 22;
    out->quiet_end_hour   = 7;
    out->critical_overrides_quiet = true;
}

static void cfg_load_from_nvs(buzzer_config_t *out) {
    buzzer_default_config(out);
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(*out);
    if (nvs_get_blob(h, NVS_KEY_BLOB, out, &sz) != ESP_OK || sz != sizeof(*out)) {
        // Either no blob or schema drift — keep defaults.
        buzzer_default_config(out);
    }
    nvs_close(h);
}

static esp_err_t cfg_save_to_nvs(const buzzer_config_t *in) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_KEY_BLOB, in, sizeof(*in));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ─── GPIO helpers ────────────────────────────────────────────────────────────

static inline void buzz_gpio_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << s_pin,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(s_pin, 0);
}

static inline void buzz_on(void)  { if (s_pin >= 0) gpio_set_level(s_pin, 1); }
static inline void buzz_off(void) { if (s_pin >= 0) gpio_set_level(s_pin, 0); }

// ─── Gate logic ──────────────────────────────────────────────────────────────

// Returns true iff the current local hour is inside [start, end) wrap-aware.
// e.g. start=22, end=7 → quiet between 22:00..06:59.
static bool in_quiet_window(uint8_t start, uint8_t end) {
    if (start == end) return false;                    // disabled window
    time_t now = time(NULL);
    if (now < 1700000000) return false;                // SNTP not synced yet — never quiet
    struct tm tm;
    localtime_r(&now, &tm);
    uint8_t h = (uint8_t)tm.tm_hour;
    return (start < end) ? (h >= start && h < end)
                         : (h >= start || h < end);    // wrap-around (e.g. 22→7)
}

// Returns the pattern that should actually play, accounting for profile.
// Returns NULL if event is gated off and should be silently dropped.
static const buzz_pattern_t *pick_pattern(buzzer_event_t evt,
                                          buzzer_profile_t prof,
                                          bool bypass_gates) {
    if (evt >= BUZZ__COUNT) return NULL;

    // Boot tone bypasses all gates — design rule.
    bool is_boot = (evt == BUZZ_BOOT_COMPLETE);

    if (!is_boot && !bypass_gates) {
        if (!s_cfg.master_enable)           return NULL;
        if (!s_cfg.alert_enable[evt])       return NULL;
        bool is_critical = (evt == BUZZ_CRITICAL_LOW);
        bool override_q  = is_critical && s_cfg.critical_overrides_quiet;
        if (!override_q && in_quiet_window(s_cfg.quiet_start_hour, s_cfg.quiet_end_hour)) {
            return NULL;
        }
    }

    if (prof == BUZZ_PROFILE_QUIET) return &QUIET_PATTERN;
    return &STANDARD[evt];
}

// ─── Pattern player ──────────────────────────────────────────────────────────

static void play_pattern(const buzz_pattern_t *pat, buzzer_profile_t prof) {
    if (!pat || pat->beat_count == 0) return;
    // Loud transforms durations at playback (1.5× on, 0.67× off).
    bool loud = (prof == BUZZ_PROFILE_LOUD);
    for (uint8_t r = 0; r < pat->repeat; r++) {
        for (uint8_t i = 0; i < pat->beat_count; i++) {
            uint16_t on_ms  = pat->beats[i].on_ms;
            uint16_t off_ms = pat->beats[i].off_ms;
            if (loud) {
                on_ms  = (uint16_t)((uint32_t)on_ms  * 3 / 2);   // ×1.5
                off_ms = (uint16_t)((uint32_t)off_ms * 2 / 3);   // ×0.67
            }
            buzz_on();
            if (on_ms)  vTaskDelay(pdMS_TO_TICKS(on_ms));
            buzz_off();
            if (off_ms) vTaskDelay(pdMS_TO_TICKS(off_ms));
        }
    }
}

static void buzzer_task(void *arg) {
    (void)arg;
    buzz_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) continue;

        xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
        buzzer_config_t cfg_snapshot = s_cfg;
        xSemaphoreGive(s_cfg_mutex);

        buzzer_profile_t prof = (msg.profile_override == BUZZ_PROFILE_USE_CONFIG)
                              ? (buzzer_profile_t)cfg_snapshot.master_profile
                              : msg.profile_override;

        const buzz_pattern_t *pat = pick_pattern(msg.evt, prof, msg.bypass_gates);
        if (pat) {
            ESP_LOGI(TAG, "play evt=%d (%s) profile=%d", msg.evt, EVENT_LABELS[msg.evt], prof);
            play_pattern(pat, prof);
        }
    }
}

// ─── Public API ──────────────────────────────────────────────────────────────

esp_err_t buzzer_init(int gpio_num) {
    if (s_initialized) return ESP_OK;
    if (gpio_num < 0) return ESP_ERR_INVALID_ARG;
    s_pin = gpio_num;

    s_cfg_mutex = xSemaphoreCreateMutex();
    if (!s_cfg_mutex) return ESP_ERR_NO_MEM;

    cfg_load_from_nvs(&s_cfg);
    buzz_gpio_init();

    s_queue = xQueueCreate(8, sizeof(buzz_msg_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    BaseType_t ok = xTaskCreate(buzzer_task, "buzzer", 3072, NULL, 5, NULL);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    s_initialized = true;
    ESP_LOGI(TAG, "buzzer ready on GPIO%d (master_enable=%d)", s_pin, s_cfg.master_enable);
    return ESP_OK;
}

void buzzer_play(buzzer_event_t evt) {
    if (!s_initialized || !s_queue) return;
    if (evt >= BUZZ__COUNT) return;
    buzz_msg_t msg = { .evt = evt, .profile_override = BUZZ_PROFILE_USE_CONFIG, .bypass_gates = false };
    xQueueSend(s_queue, &msg, 0);   // non-blocking; drop if queue full
}

void buzzer_test(buzzer_event_t evt, buzzer_profile_t profile_override) {
    if (!s_initialized || !s_queue) return;
    if (evt >= BUZZ__COUNT) return;
    buzz_msg_t msg = {
        .evt = evt,
        .profile_override = (profile_override <= BUZZ_PROFILE_LOUD) ? profile_override : BUZZ_PROFILE_USE_CONFIG,
        .bypass_gates = true,
    };
    xQueueSend(s_queue, &msg, 0);
}

void buzzer_get_config(buzzer_config_t *out) {
    if (!out) return;
    if (!s_initialized) { buzzer_default_config(out); return; }
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_cfg_mutex);
}

esp_err_t buzzer_set_config(const buzzer_config_t *in) {
    if (!in) return ESP_ERR_INVALID_ARG;
    // Clamp profile value to valid range.
    buzzer_config_t sanitized = *in;
    if (sanitized.master_profile > BUZZ_PROFILE_LOUD) {
        sanitized.master_profile = BUZZ_PROFILE_STANDARD;
    }
    if (sanitized.quiet_start_hour > 23) sanitized.quiet_start_hour = 0;
    if (sanitized.quiet_end_hour   > 23) sanitized.quiet_end_hour   = 0;

    esp_err_t err = cfg_save_to_nvs(&sanitized);
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    s_cfg = sanitized;
    xSemaphoreGive(s_cfg_mutex);
    return ESP_OK;
}

const char *buzzer_event_label(buzzer_event_t evt) {
    if (evt >= BUZZ__COUNT) return "?";
    return EVENT_LABELS[evt] ? EVENT_LABELS[evt] : "?";
}
