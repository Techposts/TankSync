/**
 * ota_manager implementation
 *
 * Manifest check flow:
 *   1. GET OTA_MANIFEST_URL  (cloud server, GH-shape JSON)
 *   2. Parse JSON: tag_name → strip "v" → compare with FIRMWARE_VERSION
 *   3. Find asset whose name starts with OTA_ASSET_PREFIX and ends with OTA_ASSET_SUFFIX
 *   4. Store download URL; set state OTA_ST_AVAILABLE
 *
 * Flash flow:
 *   1. esp_https_ota_begin() → write chunks → esp_https_ota_finish()
 *   2. esp_restart() on success
 *   3. On next boot: ota_manager_mark_valid() → prevents rollback
 */

#include "ota_manager.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "buzzer.h"                 // BUZZ_OTA_SUCCESS / BUZZ_OTA_FAILURE
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ota";

#define API_RESP_BUF_SIZE   4096
#define CHECK_INTERVAL_MS   ((uint32_t)OTA_CHECK_INTERVAL_H * 3600 * 1000)

static ota_state_t        s_state  = {0};
static SemaphoreHandle_t  s_mutex  = NULL;
static TaskHandle_t       s_task   = NULL;
static bool               s_check_requested = false;

// ── HTTP response accumulator ─────────────────────────────────────────────────
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} http_buf_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt) {
    http_buf_t *b = (http_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0 && b) {
        if (b->len + evt->data_len < b->cap - 1) {
            memcpy(b->buf + b->len, evt->data, evt->data_len);
            b->len += evt->data_len;
            b->buf[b->len] = '\0';
        }
    }
    return ESP_OK;
}

// ── Version comparison: "2.1.0" > "2.0.0" ────────────────────────────────────
// Returns true if remote > local
static bool version_newer(const char *remote, const char *local) {
    int ra = 0, rb = 0, rc = 0;
    int la = 0, lb = 0, lc = 0;
    sscanf(remote, "%d.%d.%d", &ra, &rb, &rc);
    sscanf(local,  "%d.%d.%d", &la, &lb, &lc);
    if (ra != la) return ra > la;
    if (rb != lb) return rb > lb;
    return rc > lc;
}

// ── Strip "v" prefix from tag ─────────────────────────────────────────────────
static void strip_v_prefix(const char *tag, char *out, size_t out_len) {
    const char *src = (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
    strncpy(out, src, out_len - 1);
    out[out_len - 1] = '\0';
}

// ── GitHub API check ──────────────────────────────────────────────────────────
static void do_github_check(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.status = OTA_ST_CHECKING;
    s_state.error_msg[0] = '\0';
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Checking firmware manifest: %s", OTA_MANIFEST_URL);

    char *resp_buf = malloc(API_RESP_BUF_SIZE);
    if (!resp_buf) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state.status = OTA_ST_ERROR;
        snprintf(s_state.error_msg, sizeof(s_state.error_msg), "Out of memory");
        xSemaphoreGive(s_mutex);
        return;
    }

    http_buf_t hb = { .buf = resp_buf, .len = 0, .cap = API_RESP_BUF_SIZE };

    esp_http_client_config_t http_cfg = {
        .url                = OTA_MANIFEST_URL,
        .crt_bundle_attach  = esp_crt_bundle_attach,
        .event_handler      = http_event_cb,
        .user_data          = &hb,
        .timeout_ms         = 10000,
        .user_agent         = "TankSync/" FIRMWARE_VERSION " (ESP32)",
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status_code != 200) {
        ESP_LOGE(TAG, "HTTP failed: %s, status=%d", esp_err_to_name(err), status_code);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state.status = OTA_ST_ERROR;
        snprintf(s_state.error_msg, sizeof(s_state.error_msg),
                 "HTTP error %d", status_code);
        xSemaphoreGive(s_mutex);
        free(resp_buf);
        return;
    }

    // Parse JSON
    cJSON *root = cJSON_Parse(resp_buf);
    free(resp_buf);
    if (!root) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state.status = OTA_ST_ERROR;
        snprintf(s_state.error_msg, sizeof(s_state.error_msg), "JSON parse error");
        xSemaphoreGive(s_mutex);
        return;
    }

    const char *tag = cJSON_GetStringValue(cJSON_GetObjectItem(root, "tag_name"));
    if (!tag) {
        cJSON_Delete(root);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state.status = OTA_ST_ERROR;
        snprintf(s_state.error_msg, sizeof(s_state.error_msg), "No tag_name in response");
        xSemaphoreGive(s_mutex);
        return;
    }

    char remote_ver[32];
    strip_v_prefix(tag, remote_ver, sizeof(remote_ver));

    // Find matching asset
    char asset_url[256] = {0};
    cJSON *assets = cJSON_GetObjectItem(root, "assets");
    cJSON *asset;
    cJSON_ArrayForEach(asset, assets) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(asset, "name"));
        const char *url  = cJSON_GetStringValue(cJSON_GetObjectItem(asset, "browser_download_url"));
        if (name && url &&
            strncmp(name, OTA_ASSET_PREFIX, strlen(OTA_ASSET_PREFIX)) == 0 &&
            strlen(name) > strlen(OTA_ASSET_SUFFIX) &&
            strcmp(name + strlen(name) - strlen(OTA_ASSET_SUFFIX), OTA_ASSET_SUFFIX) == 0) {
            strncpy(asset_url, url, sizeof(asset_url) - 1);
            break;
        }
    }
    cJSON_Delete(root);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_state.latest_version, remote_ver, sizeof(s_state.latest_version) - 1);
    strncpy(s_state.download_url,   asset_url,  sizeof(s_state.download_url) - 1);

    if (strlen(asset_url) == 0) {
        s_state.status = OTA_ST_ERROR;
        snprintf(s_state.error_msg, sizeof(s_state.error_msg),
                 "No matching asset in release %s", remote_ver);
        ESP_LOGW(TAG, "%s", s_state.error_msg);
    } else if (version_newer(remote_ver, FIRMWARE_VERSION)) {
        s_state.status = OTA_ST_AVAILABLE;
        ESP_LOGI(TAG, "Update available: %s → %s", FIRMWARE_VERSION, remote_ver);
    } else {
        s_state.status = OTA_ST_UP_TO_DATE;
        ESP_LOGI(TAG, "Already up to date: %s", FIRMWARE_VERSION);
    }
    xSemaphoreGive(s_mutex);
}

// ── OTA background task ───────────────────────────────────────────────────────
static void ota_task(void *arg) {
    // Initial delay: wait for WiFi to come up (30s)
    vTaskDelay(pdMS_TO_TICKS(30000));

    TickType_t last_check = xTaskGetTickCount();

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        bool do_check = s_check_requested ||
                        ((now - last_check) >= pdMS_TO_TICKS(CHECK_INTERVAL_MS));

        if (do_check) {
            s_check_requested = false;
            last_check = now;
            do_github_check();
        }

        vTaskDelay(pdMS_TO_TICKS(60000));  // sleep 1 min between loop iterations
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t ota_manager_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    memset(&s_state, 0, sizeof(s_state));
    s_state.status = OTA_ST_IDLE;

    ota_manager_mark_valid();

    xTaskCreate(ota_task, "ota", 6144, NULL, 3, &s_task);
    ESP_LOGI(TAG, "OTA manager init. Current: %s", FIRMWARE_VERSION);
    return ESP_OK;
}

esp_err_t ota_manager_check_github(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool busy = (s_state.status == OTA_ST_DOWNLOADING || s_state.status == OTA_ST_CHECKING);
    xSemaphoreGive(s_mutex);
    if (busy) {
        ESP_LOGW(TAG, "Check ignored — already %s",
                 s_state.status == OTA_ST_DOWNLOADING ? "downloading" : "checking");
        return ESP_ERR_INVALID_STATE;
    }
    s_check_requested = true;
    ESP_LOGI(TAG, "Manual manifest check requested");
    return ESP_OK;
}

esp_err_t ota_manager_flash_url(const char *url) {
    ESP_LOGI(TAG, "Flashing from: %s", url);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.status = OTA_ST_DOWNLOADING;
    s_state.progress_pct = 0;
    xSemaphoreGive(s_mutex);

    esp_http_client_config_t http_cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 60000,
        .keep_alive_enable = true,
        .user_agent        = "TankSync/" FIRMWARE_VERSION " (ESP32)",
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state.status = OTA_ST_ERROR;
        snprintf(s_state.error_msg, sizeof(s_state.error_msg),
                 "OTA begin: %s", esp_err_to_name(err));
        xSemaphoreGive(s_mutex);
        return err;
    }

    int image_size = esp_https_ota_get_image_size(ota_handle);
    int bytes_read = 0;

    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            bytes_read = esp_https_ota_get_image_len_read(ota_handle);
            if (image_size > 0) {
                int pct = (bytes_read * 100) / image_size;
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_state.progress_pct = pct;
                xSemaphoreGive(s_mutex);
                if (pct % 10 == 0) ESP_LOGI(TAG, "Progress: %d%%", pct);
            }
        } else {
            break;
        }
    }

    bool is_complete = esp_https_ota_is_complete_data_received(ota_handle);
    esp_err_t finish_err = esp_https_ota_finish(ota_handle);

    if (err != ESP_OK || !is_complete || finish_err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: perform=%s complete=%d finish=%s",
                 esp_err_to_name(err), is_complete, esp_err_to_name(finish_err));
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state.status = OTA_ST_ERROR;
        snprintf(s_state.error_msg, sizeof(s_state.error_msg), "Download failed");
        xSemaphoreGive(s_mutex);
        buzzer_play(BUZZ_OTA_FAILURE);
        return (finish_err != ESP_OK) ? finish_err : err;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.status = OTA_ST_DONE;
    s_state.progress_pct = 100;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Flash complete! Rebooting in 2s...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;  // unreachable
}

void ota_manager_mark_valid(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "Firmware marked valid (rollback cancelled)");
            // First boot after a successful OTA — announce it so users
            // know the update completed cleanly without watching logs.
            buzzer_play(BUZZ_OTA_SUCCESS);
        }
    }
}

bool ota_manager_busy(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool busy = (s_state.status == OTA_ST_CHECKING ||
                 s_state.status == OTA_ST_DOWNLOADING);
    xSemaphoreGive(s_mutex);
    return busy;
}

void ota_manager_get_state(ota_state_t *out) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_mutex);
}

// One-shot task body: snapshot the cached download_url, then run the
// blocking flash. Self-deletes when ota_manager_flash_url returns or
// device reboots (whichever comes first).
static void install_task(void *arg) {
    char url[sizeof(s_state.download_url)];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(url, s_state.download_url, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    xSemaphoreGive(s_mutex);

    if (url[0] == '\0') {
        ESP_LOGE(TAG, "install_task: no download_url cached");
        vTaskDelete(NULL);
        return;
    }
    ota_manager_flash_url(url);  // blocking; on success → esp_restart()
    vTaskDelete(NULL);
}

esp_err_t ota_manager_request_install(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool busy     = (s_state.status == OTA_ST_DOWNLOADING ||
                     s_state.status == OTA_ST_CHECKING);
    bool has_url  = (s_state.download_url[0] != '\0');
    bool eligible = (s_state.status == OTA_ST_AVAILABLE);
    xSemaphoreGive(s_mutex);

    if (busy) {
        ESP_LOGW(TAG, "install ignored — already busy");
        return ESP_ERR_INVALID_STATE;
    }
    if (!eligible) {
        // do_github_check populates download_url even when the remote isn't
        // newer (status set to UP_TO_DATE). Gate strictly on AVAILABLE so
        // the MQTT-triggered path can't downgrade an up-to-date device. The
        // local web UI gates client-side; MQTT has no client to defer to.
        ESP_LOGW(TAG, "install ignored — status is not AVAILABLE (current state has no newer release)");
        return ESP_ERR_INVALID_STATE;
    }
    if (!has_url) {
        ESP_LOGW(TAG, "install ignored — no cached download URL (run check first)");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ok = xTaskCreate(install_task, "ota_inst", 6144, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "install task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "OTA install requested → task spawned");
    return ESP_OK;
}
