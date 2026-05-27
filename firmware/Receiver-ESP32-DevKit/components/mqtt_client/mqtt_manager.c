/**
 * mqtt_manager implementation
 */

#include "mqtt_manager.h"
#include "mqtt_client.h"        // ESP-IDF MQTT (component: mqtt)
#include "esp_crt_bundle.h"    // TLS cert bundle for MQTTS
#include "transmitter_registry.h"
#include "wifi_manager.h"
#include "lora_rylr998.h"
#include "ota_manager.h"        // PWA-triggered OTA via cmd/ota_check, cmd/ota_install
#include "led_ws2812.h"         // PWA-triggered LED config via cmd/led_get, cmd/led_set
#include "buzzer.h"             // PWA-triggered buzzer config via cmd/buzzer_get, cmd/buzzer_set, cmd/buzzer_test
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"              // For set_config command payload parsing
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "mqtt";

#define NVS_NS          "mqtt"
#define MAX_TOPIC_LEN   128
#define MAX_PAYLOAD_LEN 512

static esp_mqtt_client_handle_t s_client = NULL;
static mqtt_mgr_status_t        s_status = MQTT_ST_DISABLED;
static mqtt_mgr_config_t        s_cfg    = {0};
static char                     s_dev_id[13] = {0};  // 12-char hex + null

// ── Device ID ─────────────────────────────────────────────────────────────────
static void init_device_id(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_dev_id, sizeof(s_dev_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

const char *mqtt_manager_device_id(void) { return s_dev_id; }

// ── NVS helpers ───────────────────────────────────────────────────────────────
static void load_config(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    size_t len = sizeof(s_cfg.host);
    nvs_get_str(h, "host", s_cfg.host, &len);
    uint16_t port = MQTT_DEFAULT_PORT;
    nvs_get_u16(h, "port", &port);
    s_cfg.port = port;
    len = sizeof(s_cfg.user);
    nvs_get_str(h, "user", s_cfg.user, &len);
    len = sizeof(s_cfg.pass);
    nvs_get_str(h, "pass", s_cfg.pass, &len);
    uint8_t en = 0, ha = 0;
    nvs_get_u8(h, "enabled", &en);
    nvs_get_u8(h, "ha_disc", &ha);
    s_cfg.enabled      = (en != 0);
    s_cfg.ha_discovery = (ha != 0);
    nvs_close(h);
}

static esp_err_t save_config_nvs(const mqtt_mgr_config_t *cfg) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, "host",    cfg->host);
    nvs_set_u16(h, "port",    cfg->port ? cfg->port : MQTT_DEFAULT_PORT);
    nvs_set_str(h, "user",    cfg->user);
    if (strlen(cfg->pass) > 0) nvs_set_str(h, "pass", cfg->pass); // only update if provided
    nvs_set_u8 (h, "enabled", cfg->enabled ? 1 : 0);
    nvs_set_u8 (h, "ha_disc", cfg->ha_discovery ? 1 : 0);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ── Topic builder ─────────────────────────────────────────────────────────────
static void make_topic(char *buf, size_t len,
                        const char *slug, const char *field) {
    if (slug) {
        snprintf(buf, len, MQTT_TOPIC_PREFIX "/%s/%s/%s", s_dev_id, slug, field);
    } else {
        snprintf(buf, len, MQTT_TOPIC_PREFIX "/%s/%s", s_dev_id, field);
    }
}

static void pub(const char *topic, const char *payload, int retain) {
    if (!s_client || s_status != MQTT_ST_CONNECTED) return;
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, retain);
}

// ── MQTT command handler ─────────────────────────────────────────────────────
// Handles commands received on tanksync/{device_id}/cmd/{command}
// Publishes results to tanksync/{device_id}/cmd_result/{command}
static void handle_cmd(const char *command, const char *payload) {
    char result_topic[MAX_TOPIC_LEN];
    char result[MAX_PAYLOAD_LEN];

    if (strcmp(command, "pair_start") == 0) {
        lora_set_pairing_mode(true);
        make_topic(result_topic, sizeof(result_topic), "cmd_result", "pair_start");
        pub(result_topic, "{\"ok\":true,\"status\":\"searching\"}", 0);
        ESP_LOGI(TAG, "CMD: pair_start");

    } else if (strcmp(command, "pair_stop") == 0) {
        lora_set_pairing_mode(false);
        make_topic(result_topic, sizeof(result_topic), "cmd_result", "pair_stop");
        pub(result_topic, "{\"ok\":true}", 0);
        ESP_LOGI(TAG, "CMD: pair_stop");

    } else if (strcmp(command, "remove_tx") == 0) {
        // Remove a transmitter from the registry. Payload: {"addr": <uint16>}.
        // Calls registry_remove() which soft-deletes (tombstones the entry) so
        // a returning TX with the same MAC can be resurrected via PAIR_REQ.
        // Cloud invokes this when the PWA deletes a tank — without it, the
        // cloud-side DB deletion would not propagate to the hub and the tank
        // would reappear on the next PWA refresh from the truth source.
        cJSON *j = cJSON_Parse(payload);
        uint16_t addr = j ? (uint16_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "addr")) : 0;
        if (j) cJSON_Delete(j);
        make_topic(result_topic, sizeof(result_topic), "cmd_result", "remove_tx");
        if (addr == 0) {
            pub(result_topic, "{\"ok\":false,\"error\":\"missing addr\"}", 0);
            ESP_LOGW(TAG, "CMD remove_tx: missing addr in payload");
        } else {
            // Clear retained broker state BEFORE registry_remove so the
            // (slug = "tank_<addr>") topics are still meaningful at clear-time.
            // mqtt_unpublish_tank is a no-op if MQTT is down; safe to always call.
            mqtt_unpublish_tank(addr);
            if (registry_remove(addr)) {
                // Republish the manifest so cloud can prune the orphan even
                // without seeing the per-topic empty payloads (defense-in-depth).
                mqtt_publish_registry();
                snprintf(result, sizeof(result), "{\"ok\":true,\"addr\":%d}", addr);
                pub(result_topic, result, 0);
                ESP_LOGI(TAG, "CMD remove_tx: addr=%d removed (tombstoned for MAC restore)", addr);
            } else {
                snprintf(result, sizeof(result), "{\"ok\":false,\"addr\":%d,\"error\":\"not found\"}", addr);
                pub(result_topic, result, 0);
                ESP_LOGW(TAG, "CMD remove_tx: addr=%d not in registry", addr);
            }
        }

    } else if (strcmp(command, "pair_status") == 0) {
        bool active = false; uint16_t addr = 0; char name[TX_NAME_MAX] = {0}; int time_left = 0;
        lora_get_pairing_state(&active, &addr, name, &time_left);
        bool paired = !active && addr > 0;
        snprintf(result, sizeof(result),
            "{\"active\":%s,\"paired\":%s,\"addr\":%d,\"name\":\"%s\",\"time_left\":%d}",
            active ? "true" : "false", paired ? "true" : "false",
            addr, name, time_left);
        make_topic(result_topic, sizeof(result_topic), "cmd_result", "pair_status");
        pub(result_topic, result, 0);

    } else if (strcmp(command, "refresh") == 0) {
        mqtt_publish_system();
        int n = registry_count();
        for (int i = 0; i < n; i++) mqtt_publish_tank(i);
        make_topic(result_topic, sizeof(result_topic), "cmd_result", "refresh");
        pub(result_topic, "{\"ok\":true}", 0);
        ESP_LOGI(TAG, "CMD: refresh");

    } else if (strcmp(command, "set_config") == 0) {
        // Cloud-side edit: payload is the same JSON shape that POST /api/transmitters
        // accepts on the local web UI. Mirrors that handler so PWA edits and web UI
        // edits go through the identical registry + LoRa-CONFIG path. Worked-around
        // root cause: cloud server cannot HTTP-POST to a 192.x receiver from a
        // public droplet; MQTT command channel works through any NAT.
        make_topic(result_topic, sizeof(result_topic), "cmd_result", "set_config");
        cJSON *j = cJSON_Parse(payload);
        if (!j) {
            pub(result_topic, "{\"ok\":false,\"err\":\"bad_json\"}", 0);
            ESP_LOGW(TAG, "CMD set_config: parse failed");
            return;
        }
        uint16_t addr = (uint16_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "addr"));
        const char *name_ptr = cJSON_GetStringValue(cJSON_GetObjectItem(j, "name"));
        char name_buf[TX_NAME_MAX] = {0};
        if (name_ptr) { strncpy(name_buf, name_ptr, TX_NAME_MAX - 1); }
        int min_d  = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "min_dist"));
        int max_d  = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "max_dist"));
        float cap  = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "capacity"));
        uint32_t sleep_s = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "sleep_s"));
        uint8_t  samples = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "samples"));
        cJSON *pwr_j = cJSON_GetObjectItem(j, "lora_pwr");
        uint8_t  pwr = pwr_j ? (uint8_t)cJSON_GetNumberValue(pwr_j) : 0;
        if (pwr > 22) pwr = 22;
        // Optional sensor_kind ("sr04" | "ld2413"). Absent or empty = leave
        // whatever the TX currently has. Validation is in registry_set_sensor_kind.
        const char *sk_ptr = cJSON_GetStringValue(cJSON_GetObjectItem(j, "sensor_kind"));
        char sk_buf[12] = {0};
        if (sk_ptr) { strncpy(sk_buf, sk_ptr, sizeof(sk_buf) - 1); }
        cJSON_Delete(j);

        const char *name = name_ptr ? name_buf : NULL;

        if (addr == 0) {
            pub(result_topic, "{\"ok\":false,\"err\":\"bad_addr\"}", 0);
            return;
        }
        bool updated = registry_update(addr, name, min_d, max_d, cap);
        if (!updated && registry_add(addr, name, min_d, max_d, cap) < 0) {
            pub(result_topic, "{\"ok\":false,\"err\":\"registry_full\"}", 0);
            return;
        }
        if (sleep_s >= 60) {
            registry_set_remote_config(addr, sleep_s, samples > 0 ? samples : 5, pwr);
        }
        if (sk_buf[0]) {
            if (!registry_set_sensor_kind(addr, sk_buf)) {
                pub(result_topic, "{\"ok\":false,\"err\":\"bad_sensor_kind\"}", 0);
                return;
            }
        }
        // Republish config immediately so cloud sees the new state without waiting
        // for the next periodic publish cycle.
        int idx = registry_find(addr);
        if (idx >= 0) mqtt_publish_tank(idx);

        pub(result_topic, "{\"ok\":true}", 0);
        ESP_LOGI(TAG, "CMD set_config: addr=%u sleep=%lu samples=%u pwr=%u sensor=%s",
                 (unsigned)addr, (unsigned long)sleep_s, (unsigned)samples, (unsigned)pwr,
                 sk_buf[0] ? sk_buf : "(unchanged)");

    } else if (strcmp(command, "ota_check") == 0) {
        // PWA-triggered manifest check. Non-blocking — actual fetch runs in
        // ota_task within ~60s. PWA polls cmd/ota_status to learn the result.
        make_topic(result_topic, sizeof(result_topic), "cmd_result", "ota_check");
        esp_err_t r = ota_manager_check_github();
        if (r == ESP_OK) {
            pub(result_topic, "{\"ok\":true,\"queued\":true}", 0);
        } else if (r == ESP_ERR_INVALID_STATE) {
            pub(result_topic, "{\"ok\":false,\"err\":\"busy\"}", 0);
        } else {
            snprintf(result, sizeof(result),
                     "{\"ok\":false,\"err\":\"%s\"}", esp_err_to_name(r));
            pub(result_topic, result, 0);
        }
        ESP_LOGI(TAG, "CMD ota_check: %s", esp_err_to_name(r));

    } else if (strcmp(command, "ota_status") == 0) {
        // Snapshot the OTA state for PWA polling. Status enum mirrors
        // ota_status_t in ota_manager.h.
        make_topic(result_topic, sizeof(result_topic), "cmd_result", "ota_status");
        ota_state_t st;
        ota_manager_get_state(&st);
        const char *st_str =
            st.status == OTA_ST_IDLE        ? "idle"        :
            st.status == OTA_ST_CHECKING    ? "checking"    :
            st.status == OTA_ST_AVAILABLE   ? "available"   :
            st.status == OTA_ST_DOWNLOADING ? "downloading" :
            st.status == OTA_ST_DONE        ? "done"        :
            st.status == OTA_ST_UP_TO_DATE  ? "up_to_date"  :
            st.status == OTA_ST_ERROR       ? "error"       : "unknown";
        // Escape-safe minimal JSON; latest_version + error_msg are bounded
        // strings (32 / 128) and never contain quotes in practice (set by
        // do_github_check using snprintf with controlled inputs).
        snprintf(result, sizeof(result),
            "{\"ok\":true,\"status\":\"%s\",\"current\":\"%s\",\"latest\":\"%s\",\"progress\":%d,\"error\":\"%s\"}",
            st_str, FIRMWARE_VERSION, st.latest_version,
            st.progress_pct, st.error_msg);
        pub(result_topic, result, 0);

    } else if (strcmp(command, "ota_install") == 0) {
        // Trigger flash from cached download_url (set by prior ota_check).
        // Spawns a one-shot task — the flash itself takes ~30s and would
        // otherwise stall the MQTT event loop.
        make_topic(result_topic, sizeof(result_topic), "cmd_result", "ota_install");
        esp_err_t r = ota_manager_request_install();
        if (r == ESP_OK) {
            pub(result_topic, "{\"ok\":true,\"installing\":true}", 0);
        } else if (r == ESP_ERR_INVALID_STATE) {
            pub(result_topic, "{\"ok\":false,\"err\":\"no_update_or_busy\"}", 0);
        } else {
            snprintf(result, sizeof(result),
                     "{\"ok\":false,\"err\":\"%s\"}", esp_err_to_name(r));
            pub(result_topic, result, 0);
        }
        ESP_LOGI(TAG, "CMD ota_install: %s", esp_err_to_name(r));

    } else if (strcmp(command, "led_get") == 0) {
        // 2.6.3: PWA polls current LED config + driver health for the Lights & Display panel.
        make_topic(result_topic, sizeof(result_topic), "cmd_result", "led_get");
        uint8_t count = 2, bright = 50;
        nvs_handle_t h;
        if (nvs_open("system", NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, "led_count", &count);
            nvs_get_u8(h, "led_bright", &bright);
            nvs_close(h);
        }
        led_status_t st; led_get_status(&st);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject  (root, "ok",            true);
        cJSON_AddNumberToObject(root, "count",         count);
        cJSON_AddNumberToObject(root, "brightness",    bright);
        cJSON_AddBoolToObject  (root, "initialized",   st.initialized);
        cJSON_AddBoolToObject  (root, "strip_present", st.present);
        cJSON_AddNumberToObject(root, "fail_count",    st.fail_count);
        cJSON *cols = cJSON_AddArrayToObject(root, "tank_colors");
        int n = registry_count();
        for (int i = 0; i < n; i++) {
            tx_info_t info;
            if (registry_get_info(i, &info)) {
                cJSON *c = cJSON_CreateObject();
                cJSON_AddNumberToObject(c, "addr",      info.address);
                cJSON_AddStringToObject(c, "name",      info.name);
                cJSON_AddNumberToObject(c, "color_idx", info.led_color_idx);
                cJSON_AddItemToArray(cols, c);
            }
        }
        char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root);
        pub(result_topic, json ? json : "{\"ok\":false}", 0);
        if (json) free(json);
        ESP_LOGI(TAG, "CMD led_get");

    } else if (strcmp(command, "led_set") == 0) {
        // Mirror of POST /api/led on the local web UI, callable from cloud PWA.
        make_topic(result_topic, sizeof(result_topic), "cmd_result", "led_set");
        cJSON *j = cJSON_Parse(payload);
        if (!j) { pub(result_topic, "{\"ok\":false,\"err\":\"bad_json\"}", 0); return; }

        uint8_t new_count = 0, new_bright = 0;
        nvs_handle_t h;
        if (nvs_open("system", NVS_READWRITE, &h) == ESP_OK) {
            cJSON *v;
            if ((v = cJSON_GetObjectItem(j, "count"))) {
                uint8_t c = (uint8_t)v->valueint;
                if (c == 2 || c == 8 || c == 24) {
                    nvs_set_u8(h, "led_count", c);
                    new_count = c;
                }
            }
            if ((v = cJSON_GetObjectItem(j, "brightness"))) {
                uint8_t b = (uint8_t)v->valueint;
                if (b > 0) { nvs_set_u8(h, "led_bright", b); new_bright = b; }
            }
            nvs_commit(h); nvs_close(h);
        }

        cJSON *colors = cJSON_GetObjectItem(j, "tank_colors");
        if (colors && cJSON_IsArray(colors)) {
            cJSON *item;
            cJSON_ArrayForEach(item, colors) {
                uint16_t addr = (uint16_t)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "addr"));
                int8_t   ci   = (int8_t) cJSON_GetNumberValue(cJSON_GetObjectItem(item, "color_idx"));
                if (addr > 0) registry_set_led_color(addr, ci);
            }
        }
        cJSON_Delete(j);

        if (new_count) {
            uint8_t bb = new_bright;
            if (!bb) {
                nvs_handle_t hr;
                if (nvs_open("system", NVS_READONLY, &hr) == ESP_OK) {
                    nvs_get_u8(hr, "led_bright", &bb);
                    nvs_close(hr);
                }
                if (!bb) bb = 50;
            }
            led_reinit(new_count, bb);
        } else if (new_bright) {
            led_set_brightness(new_bright);
        }

        pub(result_topic, "{\"ok\":true}", 0);
        ESP_LOGI(TAG, "CMD led_set: count=%u bright=%u", (unsigned)new_count, (unsigned)new_bright);

    } else if (strcmp(command, "display_get") == 0) {
        make_topic(result_topic, sizeof(result_topic), "cmd_result", "display_get");
        uint8_t mask = 0x1F;
        nvs_handle_t h;
        if (nvs_open("display", NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, "mask", &mask);
            nvs_close(h);
        }
        snprintf(result, sizeof(result), "{\"ok\":true,\"mask\":%d}", mask);
        pub(result_topic, result, 0);
        ESP_LOGI(TAG, "CMD display_get: mask=%d", mask);

    } else if (strcmp(command, "display_set") == 0) {
        make_topic(result_topic, sizeof(result_topic), "cmd_result", "display_set");
        cJSON *j = cJSON_Parse(payload);
        if (!j) { pub(result_topic, "{\"ok\":false,\"err\":\"bad_json\"}", 0); return; }
        cJSON *mv = cJSON_GetObjectItem(j, "mask");
        if (!mv) { cJSON_Delete(j); pub(result_topic, "{\"ok\":false,\"err\":\"missing_mask\"}", 0); return; }
        uint8_t mask = (uint8_t)cJSON_GetNumberValue(mv);
        cJSON_Delete(j);
        nvs_handle_t h;
        if (nvs_open("display", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "mask", mask);
            nvs_commit(h); nvs_close(h);
        }
        pub(result_topic, "{\"ok\":true}", 0);
        ESP_LOGI(TAG, "CMD display_set: mask=%d", mask);

    } else if (strcmp(command, "buzzer_get") == 0) {
        // Mirror of HTTP GET /api/buzzer for PWA round-trip via cloud.
        // JSON >512 B because of 14-entry alerts array — heap-print via cJSON.
        make_topic(result_topic, sizeof(result_topic), "cmd_result", "buzzer_get");
        buzzer_config_t cfg;
        buzzer_get_config(&cfg);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", true);
        cJSON_AddBoolToObject(j, "master_enable", cfg.master_enable);
        cJSON_AddNumberToObject(j, "master_profile", cfg.master_profile);
        cJSON_AddNumberToObject(j, "quiet_start_hour", cfg.quiet_start_hour);
        cJSON_AddNumberToObject(j, "quiet_end_hour", cfg.quiet_end_hour);
        cJSON_AddBoolToObject(j, "critical_overrides_quiet", cfg.critical_overrides_quiet);
        cJSON *arr = cJSON_AddArrayToObject(j, "alerts");
        for (int i = 0; i < BUZZ__COUNT; i++) {
            cJSON *a = cJSON_CreateObject();
            cJSON_AddNumberToObject(a, "event", i);
            cJSON_AddStringToObject(a, "name", buzzer_event_label((buzzer_event_t)i));
            cJSON_AddBoolToObject(a, "enabled", cfg.alert_enable[i]);
            int tier = (i <= BUZZ_TEST_BUTTON) ? 1 : (i <= BUZZ_OTA_FAILURE) ? 2 : 3;
            cJSON_AddNumberToObject(a, "tier", tier);
            cJSON_AddItemToArray(arr, a);
        }
        char *s = cJSON_PrintUnformatted(j);
        if (s) { pub(result_topic, s, 0); free(s); }
        cJSON_Delete(j);
        ESP_LOGI(TAG, "CMD buzzer_get");

    } else if (strcmp(command, "buzzer_set") == 0) {
        make_topic(result_topic, sizeof(result_topic), "cmd_result", "buzzer_set");
        cJSON *j = cJSON_Parse(payload);
        if (!j) { pub(result_topic, "{\"ok\":false,\"err\":\"bad_json\"}", 0); return; }
        buzzer_config_t cfg;
        buzzer_get_config(&cfg);
        cJSON *m = cJSON_GetObjectItem(j, "master_enable");
        if (cJSON_IsBool(m)) cfg.master_enable = cJSON_IsTrue(m);
        cJSON *mp = cJSON_GetObjectItem(j, "master_profile");
        if (cJSON_IsNumber(mp)) cfg.master_profile = (uint8_t)mp->valueint;
        cJSON *qs = cJSON_GetObjectItem(j, "quiet_start_hour");
        if (cJSON_IsNumber(qs)) cfg.quiet_start_hour = (uint8_t)qs->valueint;
        cJSON *qe = cJSON_GetObjectItem(j, "quiet_end_hour");
        if (cJSON_IsNumber(qe)) cfg.quiet_end_hour = (uint8_t)qe->valueint;
        cJSON *co = cJSON_GetObjectItem(j, "critical_overrides_quiet");
        if (cJSON_IsBool(co)) cfg.critical_overrides_quiet = cJSON_IsTrue(co);
        cJSON *arr = cJSON_GetObjectItem(j, "alerts");
        if (cJSON_IsArray(arr)) {
            cJSON *item;
            cJSON_ArrayForEach(item, arr) {
                cJSON *ev = cJSON_GetObjectItem(item, "event");
                if (!cJSON_IsNumber(ev)) continue;
                int idx = ev->valueint;
                if (idx < 0 || idx >= BUZZ__COUNT) continue;
                cJSON *en = cJSON_GetObjectItem(item, "enabled");
                if (cJSON_IsBool(en)) cfg.alert_enable[idx] = cJSON_IsTrue(en);
            }
        }
        cJSON_Delete(j);
        esp_err_t err = buzzer_set_config(&cfg);
        pub(result_topic, (err == ESP_OK) ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"persist_failed\"}", 0);
        ESP_LOGI(TAG, "CMD buzzer_set: master_enable=%d profile=%d", cfg.master_enable, cfg.master_profile);

    } else if (strcmp(command, "buzzer_test") == 0) {
        make_topic(result_topic, sizeof(result_topic), "cmd_result", "buzzer_test");
        cJSON *j = cJSON_Parse(payload);
        if (!j) { pub(result_topic, "{\"ok\":false,\"err\":\"bad_json\"}", 0); return; }
        int event = 4;  // BUZZ_TEST_BUTTON
        int profile = -1;
        cJSON *ev = cJSON_GetObjectItem(j, "event");
        if (cJSON_IsNumber(ev)) event = ev->valueint;
        cJSON *pr = cJSON_GetObjectItem(j, "profile");
        if (cJSON_IsNumber(pr)) profile = pr->valueint;
        cJSON_Delete(j);
        if (event < 0 || event >= BUZZ__COUNT) {
            pub(result_topic, "{\"ok\":false,\"err\":\"bad_event\"}", 0);
            return;
        }
        buzzer_profile_t prof_override = (profile >= 0 && profile <= BUZZ_PROFILE_LOUD)
                                        ? (buzzer_profile_t)profile : 0xff;
        buzzer_test((buzzer_event_t)event, prof_override);
        pub(result_topic, "{\"ok\":true}", 0);
        ESP_LOGI(TAG, "CMD buzzer_test: event=%d profile=%d", event, profile);

    } else if (strcmp(command, "ident_hub") == 0) {
        // Blink the hub's status LED so the user can find it physically.
        // Mirrors HTTP POST /api/v1/hub/identify.
        make_topic(result_topic, sizeof(result_topic), "cmd_result", "ident_hub");
        led_identify(LED_IDX_STATUS);
        pub(result_topic, "{\"ok\":true}", 0);
        ESP_LOGI(TAG, "CMD ident_hub");

    } else if (strcmp(command, "ident_tank") == 0) {
        // Blink a specific tank's LED. Payload: {"addr": <uint16>}.
        // Mirrors HTTP POST /api/v1/devices/<id>/identify.
        make_topic(result_topic, sizeof(result_topic), "cmd_result", "ident_tank");
        cJSON *j = cJSON_Parse(payload);
        if (!j) { pub(result_topic, "{\"ok\":false,\"err\":\"bad_json\"}", 0); return; }
        uint16_t addr = (uint16_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "addr"));
        cJSON_Delete(j);
        int idx = -1;
        for (int i = 0; i < registry_count(); i++) {
            tx_info_t info;
            if (registry_get_info(i, &info) && info.address == addr) { idx = i; break; }
        }
        if (idx < 0) {
            pub(result_topic, "{\"ok\":false,\"err\":\"unknown_addr\"}", 0);
            ESP_LOGW(TAG, "CMD ident_tank: unknown addr %u", addr);
            return;
        }
        led_identify(LED_IDX_TANK_START + idx);
        pub(result_topic, "{\"ok\":true}", 0);
        ESP_LOGI(TAG, "CMD ident_tank: addr=%u", addr);

    } else if (strcmp(command, "reboot") == 0) {
        // Clean restart. Requires {"confirm":true} so accidental dispatches
        // are no-ops. Mirrors HTTP POST /api/v1/hub/reboot.
        make_topic(result_topic, sizeof(result_topic), "cmd_result", "reboot");
        cJSON *j = cJSON_Parse(payload);
        bool confirmed = false;
        if (j) {
            cJSON *cv = cJSON_GetObjectItem(j, "confirm");
            confirmed = cv && cJSON_IsTrue(cv);
            cJSON_Delete(j);
        }
        if (!confirmed) {
            pub(result_topic, "{\"ok\":false,\"err\":\"confirm_required\"}", 0);
            ESP_LOGW(TAG, "CMD reboot: rejected — missing confirm:true");
            return;
        }
        pub(result_topic, "{\"ok\":true,\"rebooting\":true}", 0);
        ESP_LOGW(TAG, "CMD reboot: hub will restart in ~1s");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();

    } else if (strcmp(command, "wifi_forget") == 0) {
        // Destructive: clears WiFi credentials and drops device into AP mode for re-setup.
        // Customer must be physically near the hub afterwards (cellular PWA can't reach AP).
        // Requires {"confirm":true} so accidental reissue does nothing.
        make_topic(result_topic, sizeof(result_topic), "cmd_result", "wifi_forget");
        cJSON *j = cJSON_Parse(payload);
        if (!j) { pub(result_topic, "{\"ok\":false,\"err\":\"bad_json\"}", 0); return; }
        cJSON *cv = cJSON_GetObjectItem(j, "confirm");
        bool confirmed = cv && cJSON_IsTrue(cv);
        cJSON_Delete(j);
        if (!confirmed) {
            pub(result_topic, "{\"ok\":false,\"err\":\"confirm_required\"}", 0);
            ESP_LOGW(TAG, "CMD wifi_forget: rejected — missing confirm:true");
            return;
        }
        pub(result_topic, "{\"ok\":true,\"rebooting\":true}", 0);
        ESP_LOGW(TAG, "CMD wifi_forget: clearing credentials, rebooting into AP mode");
        vTaskDelay(pdMS_TO_TICKS(2000));  // let MQTT publish drain before WiFi drops
        wifi_manager_forget();
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();

    } else {
        ESP_LOGW(TAG, "Unknown command: %s", command);
    }
}

// ── MQTT event handler ────────────────────────────────────────────────────────
static void on_mqtt_event(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "Connected → %s:%d", s_cfg.host, s_cfg.port);
            s_status = MQTT_ST_CONNECTED;

            char topic[MAX_TOPIC_LEN];
            make_topic(topic, sizeof(topic), NULL, "status");
            pub(topic, "online", 1);

            // Subscribe to command channel
            char cmd_topic[MAX_TOPIC_LEN];
            make_topic(cmd_topic, sizeof(cmd_topic), "cmd", "#");
            esp_mqtt_client_subscribe(s_client, cmd_topic, 1);
            ESP_LOGI(TAG, "Subscribed to %s", cmd_topic);

            mqtt_publish_system();
            if (s_cfg.ha_discovery) mqtt_publish_ha_discovery();

            int n = registry_count();
            for (int i = 0; i < n; i++) mqtt_publish_tank(i);

            // Publish the registry manifest LAST so cloud subscribers see all
            // per-tank topics arrive first, then learn the canonical truth.
            // The retained manifest is what the cloud uses to prune orphans.
            mqtt_publish_registry();
            break;
        }
        case MQTT_EVENT_DATA: {
            // Parse incoming command messages
            if (!ev->topic || ev->topic_len == 0) break;
            char topic_buf[MAX_TOPIC_LEN];
            int tlen = ev->topic_len < (int)sizeof(topic_buf) - 1 ? ev->topic_len : (int)sizeof(topic_buf) - 1;
            memcpy(topic_buf, ev->topic, tlen);
            topic_buf[tlen] = '\0';

            char payload_buf[MAX_PAYLOAD_LEN];
            int plen = ev->data_len < (int)sizeof(payload_buf) - 1 ? ev->data_len : (int)sizeof(payload_buf) - 1;
            if (ev->data && plen > 0) { memcpy(payload_buf, ev->data, plen); }
            payload_buf[plen] = '\0';

            // Check if this is a command: tanksync/{device_id}/cmd/{command}
            char prefix[MAX_TOPIC_LEN];
            make_topic(prefix, sizeof(prefix), "cmd", "");
            size_t prefix_len = strlen(prefix);
            if (strncmp(topic_buf, prefix, prefix_len) == 0) {
                const char *cmd = topic_buf + prefix_len;
                handle_cmd(cmd, payload_buf);
            }
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected");
            s_status = MQTT_ST_DISCONNECTED;
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "Error");
            s_status = MQTT_ST_ERROR;
            break;
        default:
            break;
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t mqtt_manager_init(void) {
    init_device_id();
    load_config();
    // Hook the registry membership callback. Any add/remove/restore/clear
    // in transmitter_registry now triggers mqtt_publish_registry(), which
    // emits the retained manifest the cloud reconciler uses. The callback
    // pattern avoids a circular component dependency (mqtt_client already
    // REQUIRES lora_rylr998 — so transmitter_registry can't directly call
    // into mqtt_client without inverting that relationship).
    registry_set_change_callback(mqtt_publish_registry);
    ESP_LOGI(TAG, "device_id=%s, broker=%s:%d, enabled=%d ha=%d",
             s_dev_id, s_cfg.host, s_cfg.port,
             s_cfg.enabled, s_cfg.ha_discovery);
    return ESP_OK;
}

void mqtt_manager_start(void) {
    if (!s_cfg.enabled || strlen(s_cfg.host) == 0) return;
    if (s_client) { esp_mqtt_client_reconnect(s_client); return; }

    char lwt_topic[MAX_TOPIC_LEN];
    make_topic(lwt_topic, sizeof(lwt_topic), NULL, "status");

    char client_id[32];
    snprintf(client_id, sizeof(client_id), "tanksync_%s", s_dev_id);

    esp_mqtt_client_config_t cfg = {
        .broker.address.hostname         = s_cfg.host,
        .broker.address.port             = s_cfg.port ? s_cfg.port : MQTT_DEFAULT_PORT,
        .broker.address.transport        = (s_cfg.port == 8883) ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP,
        .broker.verification.crt_bundle_attach = (s_cfg.port == 8883) ? esp_crt_bundle_attach : NULL,
        .credentials.client_id           = client_id,
        .credentials.username            = strlen(s_cfg.user) ? s_cfg.user : NULL,
        .credentials.authentication.password = strlen(s_cfg.pass) ? s_cfg.pass : NULL,
        .session.keepalive               = MQTT_KEEPALIVE_S,
        .session.last_will.topic         = lwt_topic,
        .session.last_will.msg           = "offline",
        .session.last_will.msg_len       = 7,
        .session.last_will.qos           = 1,
        .session.last_will.retain        = 1,
        .network.reconnect_timeout_ms    = MQTT_RECONNECT_BASE_MS,
        .network.timeout_ms              = 10000,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) { ESP_LOGE(TAG, "Init failed"); s_status = MQTT_ST_ERROR; return; }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL);
    s_status = MQTT_ST_CONNECTING;
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "Connecting → %s:%d", s_cfg.host, s_cfg.port);
}

void mqtt_manager_stop(void) {
    if (!s_client) return;
    if (s_status == MQTT_ST_CONNECTED) {
        char topic[MAX_TOPIC_LEN];
        make_topic(topic, sizeof(topic), NULL, "status");
        pub(topic, "offline", 1);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_status = MQTT_ST_DISABLED;
}

void mqtt_publish_tank(int idx) {
    if (s_status != MQTT_ST_CONNECTED) return;

    tx_info_t info;
    tx_data_t data;
    if (!registry_get_info(idx, &info)) return;
    if (!registry_get_data(idx, &data)) return;
    if (!info.enabled) return;

    // Publish under address-keyed slug ("tank_<addr>") not the user-given name.
    // This way the cloud server can ingest and auto-create devices regardless of
    // what the customer named the tank in firmware UI. Friendly names live in
    // the cloud devices.name column and don't need to round-trip through topics.
    char slug[TX_NAME_MAX];
    snprintf(slug, sizeof(slug), "tank_%d", info.address);

    char topic[MAX_TOPIC_LEN], val[32];

    snprintf(val, sizeof(val), "%d", data.water_pct);
    make_topic(topic, sizeof(topic), slug, "water_pct");
    pub(topic, val, 1);

    snprintf(val, sizeof(val), "%.1f", data.water_liters);
    make_topic(topic, sizeof(topic), slug, "water_liters");
    pub(topic, val, 1);

    snprintf(val, sizeof(val), "%d", data.raw_dist_cm);
    make_topic(topic, sizeof(topic), slug, "distance_cm");
    pub(topic, val, 0);

    snprintf(val, sizeof(val), "%d", data.battery_pct);
    make_topic(topic, sizeof(topic), slug, "battery_pct");
    pub(topic, val, 1);

    snprintf(val, sizeof(val), "%.2f", data.battery_voltage);
    make_topic(topic, sizeof(topic), slug, "battery_v");
    pub(topic, val, 1);

    snprintf(val, sizeof(val), "%d", data.rssi);
    make_topic(topic, sizeof(topic), slug, "rssi");
    pub(topic, val, 0);

    make_topic(topic, sizeof(topic), slug, "state");
    pub(topic, registry_state_str(data.state), 1);

    // Sensor health: published retained so the cloud DB + PWA know whether the
    // last reading is trustworthy. ON means the TX is alive but its ultrasonic
    // sensor failed to echo on the last read — water_pct shown is the prior
    // good reading, not current. PWA shows a sensor-error badge.
    make_topic(topic, sizeof(topic), slug, "sensor_error");
    pub(topic, data.sensor_error ? "ON" : "OFF", 1);

    // Sensor stuck (rx-v2.8.3+): variance window detected a constant reading
    // across 20 samples. Catches defective ultrasonic sensors that report
    // their min-range constant regardless of actual water level — the
    // sensor_error/dist=0 safety net misses this because the value looks
    // plausible. PWA shows a separate "stuck" badge alongside sensor_error.
    make_topic(topic, sizeof(topic), slug, "sensor_stuck");
    pub(topic, data.sensor_stuck ? "ON" : "OFF", 1);

    // Power telemetry (TX v2.0.4+) — published only when TX has reported a real mode.
    // '?' (unknown / pre-v2.0.4 TX) is skipped to avoid polluting topics for old hardware.
    if (data.power_mode == 'v' || data.power_mode == 'i' || data.power_mode == 'n') {
        const char *pmode_str = (data.power_mode == 'i') ? "ina219"
                              : (data.power_mode == 'v') ? "voltage"
                              : "none";
        make_topic(topic, sizeof(topic), slug, "power_mode");
        pub(topic, pmode_str, 1);

        snprintf(val, sizeof(val), "%ld", (long)data.current_ma);
        make_topic(topic, sizeof(topic), slug, "current_ma");
        pub(topic, val, 1);

        snprintf(val, sizeof(val), "%ld", (long)data.power_mw);
        make_topic(topic, sizeof(topic), slug, "power_mw");
        pub(topic, val, 1);

        // HA-style binary sensor convention: ON / OFF
        make_topic(topic, sizeof(topic), slug, "charging");
        pub(topic, data.charging ? "ON" : "OFF", 1);
    }

    // Config fields — published retained=1 so the cloud DB stays in sync with
    // RX registry regardless of edit origin (RX web UI, PWA via set_config cmd,
    // or future TX-driven changes). Without these, edits made on the local web
    // UI never round-trip back to the cloud, leaving PWA TankDetail showing
    // stale values forever.
    make_topic(topic, sizeof(topic), slug, "name");
    pub(topic, info.name, 1);

    snprintf(val, sizeof(val), "%d", info.min_dist_cm);
    make_topic(topic, sizeof(topic), slug, "min_dist");
    pub(topic, val, 1);

    snprintf(val, sizeof(val), "%d", info.max_dist_cm);
    make_topic(topic, sizeof(topic), slug, "max_dist");
    pub(topic, val, 1);

    snprintf(val, sizeof(val), "%.1f", info.capacity_liters);
    make_topic(topic, sizeof(topic), slug, "capacity");
    pub(topic, val, 1);

    snprintf(val, sizeof(val), "%lu", (unsigned long)info.sleep_s);
    make_topic(topic, sizeof(topic), slug, "sleep_s");
    pub(topic, val, 1);

    snprintf(val, sizeof(val), "%u", (unsigned)info.samples);
    make_topic(topic, sizeof(topic), slug, "samples");
    pub(topic, val, 1);

    snprintf(val, sizeof(val), "%u", (unsigned)info.lora_pwr);
    make_topic(topic, sizeof(topic), slug, "lora_pwr");
    pub(topic, val, 1);

    // sensor_kind — what RX has QUEUED for this TX (the user's choice, what
    // gets pushed in the next SET frame). Empty = no preference recorded.
    make_topic(topic, sizeof(topic), slug, "sensor_kind");
    pub(topic, info.sensor_kind, 1);

    // active_sensor — what the TX is ACTUALLY running, reported in TANK
    // packets (since TX v2.0.15). Empty if TX firmware doesn't declare it.
    make_topic(topic, sizeof(topic), slug, "active_sensor");
    pub(topic, data.active_sensor, 1);

    // TX firmware version — published only when TX has actually reported one.
    // Empty/zero means pre-power-telemetry TX or never received the version
    // packet yet; suppress to avoid clobbering a previously-known value.
    if (data.fw_version[0] != '\0') {
        make_topic(topic, sizeof(topic), slug, "fw");
        pub(topic, data.fw_version, 1);
    }
}

// Mirror of mqtt_publish_tank() field list — every retained topic gets cleared
// with an empty payload. The non-retained ones (distance_cm, rssi) are skipped:
// they have no persistent state on the broker, so nothing to clear.
void mqtt_unpublish_tank(uint16_t addr) {
    if (s_status != MQTT_ST_CONNECTED) return;
    char slug[TX_NAME_MAX];
    snprintf(slug, sizeof(slug), "tank_%d", addr);
    static const char *retained_fields[] = {
        "water_pct", "water_liters", "battery_pct", "battery_v", "state",
        "sensor_error",
        "power_mode", "current_ma", "power_mw", "charging",
        "name", "min_dist", "max_dist", "capacity", "sleep_s", "samples",
        "lora_pwr", "sensor_kind", "active_sensor", "fw",
    };
    char topic[MAX_TOPIC_LEN];
    for (size_t i = 0; i < sizeof(retained_fields) / sizeof(retained_fields[0]); i++) {
        make_topic(topic, sizeof(topic), slug, retained_fields[i]);
        // Empty payload + retain=1 instructs the broker to delete the retained
        // record for this topic per MQTT spec §3.3.1.3.
        pub(topic, "", 1);
    }
    ESP_LOGI(TAG, "Cleared %zu retained topics for tank_%d",
             sizeof(retained_fields) / sizeof(retained_fields[0]), addr);
}

void mqtt_publish_registry(void) {
    if (s_status != MQTT_ST_CONNECTED) return;

    // Build {"v":1,"count":N,"netid":X,"freq_hz":Y,"addrs":[{...}]} payload.
    // The cloud reconciler treats this as the source of truth — any cloud DB
    // row whose lora_address isn't in this list is an orphan and gets pruned.
    // The "v" field is a forward-compat sentinel — if we ever change the
    // schema, future cloud can dispatch on version. netid + freq_hz are
    // surfaced in PWA Settings so multi-hub users can confirm radio isolation.
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    int n = registry_count();
    cJSON_AddNumberToObject(root, "count", n);
    // Read current LoRa NETID + freq from NVS (the registry doesn't track
    // these — they live in the lora namespace). Failure is benign; cloud
    // accepts manifests without these fields.
    {
        nvs_handle_t h;
        // NVS_NS_LORA lives in main/config.h; we don't pull main into this
        // component (the lora namespace name is a stable protocol-level string).
        if (nvs_open("lora", NVS_READONLY, &h) == ESP_OK) {
            uint8_t  netid = 0;
            uint32_t freq  = 0;
            if (nvs_get_u8(h, "netid", &netid) == ESP_OK && netid > 0) {
                cJSON_AddNumberToObject(root, "netid", netid);
            }
            if (nvs_get_u32(h, "freq", &freq) == ESP_OK && freq > 0) {
                cJSON_AddNumberToObject(root, "freq_hz", (double)freq);
            }
            nvs_close(h);
        }
    }
    cJSON *arr = cJSON_AddArrayToObject(root, "addrs");
    for (int i = 0; i < n; i++) {
        tx_info_t info;
        if (!registry_get_info(i, &info)) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "addr", info.address);
        const uint8_t *m = info.mac;
        bool has_mac = false;
        for (int b = 0; b < 6; b++) if (m[b]) { has_mac = true; break; }
        if (has_mac) {
            char machex[13];
            snprintf(machex, sizeof(machex), "%02x%02x%02x%02x%02x%02x",
                     m[0], m[1], m[2], m[3], m[4], m[5]);
            cJSON_AddStringToObject(item, "mac", machex);
        }
        cJSON_AddItemToArray(arr, item);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    char topic[MAX_TOPIC_LEN];
    make_topic(topic, sizeof(topic), "registry", "devices");
    pub(topic, json, 1);  // RETAINED so cloud restart picks up the latest manifest
    ESP_LOGI(TAG, "Registry manifest published: %d device(s)", n);
    free(json);
}

void mqtt_publish_system(void) {
    if (s_status != MQTT_ST_CONNECTED) return;

    char topic[MAX_TOPIC_LEN], val[32];

    make_topic(topic, sizeof(topic), "system", "ip");
    pub(topic, wifi_manager_ip(), 1);

    make_topic(topic, sizeof(topic), "system", "version");
    pub(topic, FIRMWARE_VERSION, 1);

    snprintf(val, sizeof(val), "%lu",
             (unsigned long)(esp_timer_get_time() / 1000000LL));
    make_topic(topic, sizeof(topic), "system", "uptime");
    pub(topic, val, 0);

    snprintf(val, sizeof(val), "%d", wifi_manager_rssi());
    make_topic(topic, sizeof(topic), "system", "wifi_rssi");
    pub(topic, val, 0);
}

void mqtt_publish_ha_discovery(void) {
    if (s_status != MQTT_ST_CONNECTED) return;

    int count = registry_count();
    for (int i = 0; i < count; i++) {
        tx_info_t info;
        if (!registry_get_info(i, &info) || !info.enabled) continue;

        char slug[TX_NAME_MAX];
        registry_sanitize_name(info.name, slug, sizeof(slug));
        if (strlen(slug) == 0) snprintf(slug, sizeof(slug), "tank_%d", info.address);

        char uid_base[64];
        snprintf(uid_base, sizeof(uid_base), "tanksync_%s_%s", s_dev_id, slug);

        char state_base[MAX_TOPIC_LEN];
        snprintf(state_base, sizeof(state_base),
                 MQTT_TOPIC_PREFIX "/%s/%s", s_dev_id, slug);

        char avail_topic[MAX_TOPIC_LEN];
        make_topic(avail_topic, sizeof(avail_topic), NULL, "status");

        // Sensors to announce
        static const struct {
            const char *field;
            const char *friendly;
            const char *unit;
            const char *dev_class;
            const char *icon;
        } sensors[] = {
            { "water_pct",    "Water Level",    "%",   "moisture",       "mdi:water-percent" },
            { "water_liters", "Water Volume",   "L",   NULL,             "mdi:water"         },
            { "battery_pct",  "Battery",        "%",   "battery",        NULL                },
            { "battery_v",    "Battery Voltage","V",   "voltage",        NULL                },
            { "rssi",         "LoRa RSSI",      "dBm", "signal_strength",NULL                },
            { "state",        "Status",         NULL,  NULL,             "mdi:access-point"  },
        };

        for (int s = 0; s < (int)(sizeof(sensors)/sizeof(sensors[0])); s++) {
            char ha_topic[MAX_TOPIC_LEN];
            snprintf(ha_topic, sizeof(ha_topic),
                     "homeassistant/sensor/%s_%s/config", uid_base, sensors[s].field);

            // Build JSON config payload
            char *buf = malloc(MAX_PAYLOAD_LEN);
            if (!buf) continue;

            char entity_name[80];
            snprintf(entity_name, sizeof(entity_name),
                     "%s %s", info.name, sensors[s].friendly);

            int pos = 0;
            pos += snprintf(buf + pos, MAX_PAYLOAD_LEN - pos,
                "{\"name\":\"%s\","
                "\"unique_id\":\"%s_%s\","
                "\"state_topic\":\"%s/%s\","
                "\"availability_topic\":\"%s\","
                "\"payload_available\":\"online\","
                "\"payload_not_available\":\"offline\",",
                entity_name,
                uid_base, sensors[s].field,
                state_base, sensors[s].field,
                avail_topic);

            if (sensors[s].unit && pos < MAX_PAYLOAD_LEN - 64)
                pos += snprintf(buf + pos, MAX_PAYLOAD_LEN - pos,
                    "\"unit_of_measurement\":\"%s\",", sensors[s].unit);
            if (sensors[s].dev_class && pos < MAX_PAYLOAD_LEN - 64)
                pos += snprintf(buf + pos, MAX_PAYLOAD_LEN - pos,
                    "\"device_class\":\"%s\",", sensors[s].dev_class);
            if (sensors[s].icon && pos < MAX_PAYLOAD_LEN - 64)
                pos += snprintf(buf + pos, MAX_PAYLOAD_LEN - pos,
                    "\"icon\":\"%s\",", sensors[s].icon);

            if (pos < MAX_PAYLOAD_LEN - 128)
                pos += snprintf(buf + pos, MAX_PAYLOAD_LEN - pos,
                    "\"device\":{"
                    "\"identifiers\":[\"%s\"],"
                    "\"name\":\"%s\","
                    "\"model\":\"TankSync v2\","
                    "\"manufacturer\":\"TankSync\","
                    "\"sw_version\":\"%s\"}}",
                    uid_base, info.name, FIRMWARE_VERSION);

            esp_mqtt_client_publish(s_client, ha_topic, buf, pos, 1, 1);
            free(buf);
        }
        ESP_LOGI(TAG, "HA Discovery: '%s' (%s)", info.name, uid_base);
    }
}

mqtt_mgr_status_t mqtt_manager_status(void) { return s_status; }

bool mqtt_manager_is_linked(void) {
    return s_cfg.enabled && s_cfg.user[0] != '\0' && s_cfg.pass[0] != '\0';
}

void mqtt_manager_get_config(mqtt_mgr_config_t *out) {
    *out = s_cfg;
    memset(out->pass, 0, sizeof(out->pass));  // never expose password
}

esp_err_t mqtt_manager_set_config(const mqtt_mgr_config_t *cfg) {
    esp_err_t err = save_config_nvs(cfg);
    if (err != ESP_OK) return err;
    // Preserve existing password if new one is empty
    if (strlen(cfg->pass) > 0) {
        strncpy(s_cfg.pass, cfg->pass, sizeof(s_cfg.pass) - 1);
    }
    strncpy(s_cfg.host,  cfg->host, sizeof(s_cfg.host) - 1);
    s_cfg.port         = cfg->port;
    strncpy(s_cfg.user, cfg->user, sizeof(s_cfg.user) - 1);
    s_cfg.enabled      = cfg->enabled;
    s_cfg.ha_discovery = cfg->ha_discovery;

    mqtt_manager_stop();
    if (s_cfg.enabled && strlen(s_cfg.host) > 0 &&
        wifi_manager_status() == WIFI_ST_CONNECTED) {
        mqtt_manager_start();
    }
    return ESP_OK;
}
