// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

/**
 * wifi_ota — WiFi AP + OTA upload + device settings for TankSync Transmitter
 */

#include "wifi_ota.h"
#include "battery_monitor.h"      /* power_mode_t + power_get_override / set */
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_ota";

#define AP_SSID         "TankSync-Update"
#define AP_MAX_CONN     2
#define TIMEOUT_US      (5LL * 60 * 1000000)  // 5 minutes

static int64_t s_last_activity_us = 0;
static int     s_led_gpio = 8;
static char    s_ap_ssid[32] = AP_SSID;

static void touch_activity(void) {
    s_last_activity_us = esp_timer_get_time();
}

// ── HTML page ────────────────────────────────────────────────────────────────
static const char PAGE_HTML[] =
"<!DOCTYPE html><html><head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1,user-scalable=no'>"
"<title>TankSync TX</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:'Courier New',monospace;background:#000;color:#ffb000;"
"min-height:100vh;padding:.8rem;font-size:13px;text-transform:uppercase;text-shadow:0 0 2px #ffb000}"
"h1{text-align:center;font-size:1.2rem;letter-spacing:2px;padding:.6rem 0;border-bottom:2px solid #ffb000;margin-bottom:.8rem}"
".card{border:1px solid #ffb000;padding:.8rem;margin-bottom:.8rem}"
".card-title{font-size:.85rem;font-weight:bold;margin-bottom:.5rem;padding-bottom:.3rem;border-bottom:1px solid #333}"
".row{display:flex;justify-content:space-between;font-size:.8rem;padding:.15rem 0}"
".row .v{color:#fff}"
"label{display:block;font-size:.75rem;color:#aa8800;margin:.5rem 0 .2rem}"
"input[type=number]{width:100%;padding:.4rem;background:#111;border:1px solid #ffb000;color:#ffb000;"
"font-family:'Courier New',monospace;font-size:.85rem}"
"input[type=file]{color:#ffb000;font-family:'Courier New',monospace;font-size:.8rem;margin:.3rem 0}"
".btn{display:block;width:100%;padding:.6rem;border:1px solid #ffb000;background:#000;color:#ffb000;"
"font-family:'Courier New',monospace;font-size:.9rem;font-weight:bold;cursor:pointer;margin-top:.5rem;"
"text-transform:uppercase;letter-spacing:1px}"
".btn:active{background:#ffb000;color:#000}"
".btn:disabled{opacity:.3}"
".bar-wrap{height:14px;border:1px solid #ffb000;margin-top:.5rem;display:none}"
".bar-fill{height:100%;background:#ffb000;width:0%;transition:width .3s}"
".bar-txt{text-align:center;font-size:.7rem;margin-top:.2rem}"
".msg{padding:.4rem;text-align:center;margin-top:.4rem;font-size:.8rem;display:none}"
".msg-ok{border:1px solid #0f0;color:#0f0;display:block}"
".msg-err{border:1px solid #f00;color:#f00;display:block}"
"</style></head><body>"
"<h1>[ TANKSYNC TX ]</h1>"

"<div class='card'>"
"<div class='card-title'>SYSTEM</div>"
"<div class='row'>FIRMWARE <span class='v' id='ver'>--</span></div>"
"<div class='row'>ADDRESS <span class='v' id='addr'>--</span></div>"
"<div class='row'>SLEEP <span class='v' id='slp'>--</span>S</div>"
"<div class='row'>SAMPLES <span class='v' id='smp'>--</span></div>"
"<div class='row'>POWER <span class='v' id='pmd'>--</span></div>"
"</div>"

"<div class='card'>"
"<div class='card-title'>SETTINGS</div>"
"<label>SLEEP INTERVAL (SEC)</label>"
"<input type='number' id='sleep_s' min='60' max='86400' value='300'>"
"<label>SENSOR SAMPLES</label>"
"<input type='number' id='samples' min='3' max='20' value='5'>"
"<label>POWER SENSOR</label>"
"<select id='power_override' style='width:100%;padding:.5rem;background:#000;color:#0f0;border:1px solid #0f0;font-family:monospace'>"
"<option value='auto'>AUTO-DETECT (RECOMMENDED)</option>"
"<option value='ina219'>FORCE INA219 (I&sup2;C 0X40)</option>"
"<option value='voltage'>FORCE VOLTAGE DIVIDER (ADC)</option>"
"<option value='disabled'>DISABLED (NO POWER MONITORING)</option>"
"</select>"
"<div style='font-size:.7rem;opacity:.7;margin-top:.25rem'>CHANGE TAKES EFFECT ON NEXT BOOT</div>"
"<button class='btn' onclick='saveSettings()'>SAVE</button>"
"<div class='msg' id='cfg-status'></div>"
"</div>"

"<div class='card'>"
"<div class='card-title'>FIRMWARE UPDATE</div>"
"<input type='file' id='fw-file' accept='.bin'>"
"<button class='btn' id='upload-btn' onclick='uploadFw()'>UPLOAD + FLASH</button>"
"<div class='bar-wrap' id='progress'><div class='bar-fill' id='pbar'></div></div>"
"<div class='bar-txt' id='ptxt'></div>"
"<div class='msg' id='ota-status'></div>"
"</div>"

"<div class='card'>"
"<div class='card-title'>FACTORY RESET</div>"
"<div style='font-size:.75rem;color:#aa8800;margin-bottom:.5rem'>ERASES ALL SETTINGS (PAIRING, LORA CONFIG, SLEEP)</div>"
"<button class='btn' style='border-color:#f00;color:#f00' onclick='factoryReset()'>FACTORY RESET</button>"
"<div class='msg' id='rst-status'></div>"
"</div>"

"<script>"
// Load device info on page load
"fetch('/api/info').then(r=>r.json()).then(d=>{"
"document.getElementById('ver').textContent=d.version;"
"document.getElementById('addr').textContent=d.address;"
"document.getElementById('slp').textContent=d.sleep_s;"
"document.getElementById('smp').textContent=d.samples;"
"document.getElementById('sleep_s').value=d.sleep_s;"
"document.getElementById('samples').value=d.samples;"
// Power sensor: shows the saved override (active value applies after reboot)
"var po=d.power_override||'auto';"
"var pmd=(po==='auto')?'AUTO-DETECT':('FORCED '+po.toUpperCase());"
"document.getElementById('pmd').textContent=pmd;"
"document.getElementById('power_override').value=po;"
"});"

// Save settings
"function saveSettings(){"
"var s=document.getElementById('sleep_s').value;"
"var m=document.getElementById('samples').value;"
"var po=document.getElementById('power_override').value;"
"var st=document.getElementById('cfg-status');"
"fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({sleep_s:parseInt(s),samples:parseInt(m),power_override:po})}).then(r=>r.json()).then(d=>{"
"st.className='msg '+(d.ok?'msg-ok':'msg-err');"
"st.textContent=d.ok?'SETTINGS SAVED — POWER MODE APPLIES ON NEXT BOOT':'ERROR: '+d.error;"
"document.getElementById('slp').textContent=s;"
"document.getElementById('smp').textContent=m;"
"}).catch(e=>{st.className='msg msg-err';st.textContent='Failed: '+e;});"
"}"

// Upload firmware
"function uploadFw(){"
"var f=document.getElementById('fw-file').files[0];"
"if(!f){alert('Select a .bin file first');return;}"
"var btn=document.getElementById('upload-btn');"
"var prog=document.getElementById('progress');"
"var bar=document.getElementById('pbar');"
"var txt=document.getElementById('ptxt');"
"var st=document.getElementById('ota-status');"
"btn.disabled=true;prog.style.display='block';st.style.display='none';"
"var xhr=new XMLHttpRequest();"
"xhr.upload.onprogress=function(e){"
"if(e.lengthComputable){var p=Math.round(e.loaded*100/e.total);"
"bar.style.width=p+'%';txt.textContent=p+'% ('+Math.round(e.loaded/1024)+'KB)';}"
"};"
"xhr.onload=function(){"
"if(xhr.status===200){"
"st.className='msg msg-ok';st.textContent='UPDATE OK — REBOOTING...';"
"}else{"
"st.className='msg msg-err';st.textContent='Failed: '+xhr.responseText;"
"btn.disabled=false;}"
"};"
"xhr.onerror=function(){st.className='msg msg-err';"
"st.textContent='Upload failed (connection lost)';btn.disabled=false;};"
"xhr.open('POST','/api/ota');xhr.send(f);"
"}"

"function factoryReset(){"
"if(!confirm('ERASE ALL SETTINGS? Device will unpair and reboot.'))return;"
"var st=document.getElementById('rst-status');"
"fetch('/api/reset',{method:'POST'}).then(r=>r.json()).then(d=>{"
"st.className='msg msg-ok';st.textContent='RESET COMPLETE — REBOOTING...';"
"}).catch(e=>{st.className='msg msg-err';st.textContent='FAILED';});"
"}"
"</script></body></html>";

// ── API: GET /api/info ───────────────────────────────────────────────────────
static esp_err_t handle_info(httpd_req_t *req) {
    touch_activity();
    uint32_t sleep_s = 300;
    uint8_t  samples = 5;
    uint16_t address = 0;
    char     version[32] = "unknown";

    // Read NVS settings
    nvs_handle_t h;
    if (nvs_open("system", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "sleep_s", &sleep_s);
        nvs_get_u8(h, "samples", &samples);
        nvs_close(h);
    }
    if (nvs_open("lora", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, "my_addr", &address);
        nvs_close(h);
    }

    // Get version from app desc
    const esp_app_desc_t *app = esp_app_get_description();
    if (app) strncpy(version, app->version, sizeof(version) - 1);

    // Read persisted power-monitor override (default "auto")
    char power_override[16] = "auto";
    power_get_override(power_override, sizeof(power_override));

    char json[320];
    snprintf(json, sizeof(json),
        "{\"version\":\"%s\",\"address\":%d,\"sleep_s\":%lu,\"samples\":%d,"
        "\"power_override\":\"%s\"}",
        version, (int)address, (unsigned long)sleep_s, (int)samples,
        power_override);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

// ── API: POST /api/config ────────────────────────────────────────────────────
static esp_err_t handle_config(httpd_req_t *req) {
    touch_activity();
    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    // Simple JSON parse (no library needed for 2 fields)
    uint32_t sleep_s = 300;
    uint8_t  samples = 5;
    char *p;
    if ((p = strstr(buf, "\"sleep_s\"")) != NULL) {
        p = strchr(p, ':');
        if (p) sleep_s = (uint32_t)atoi(p + 1);
    }
    if ((p = strstr(buf, "\"samples\"")) != NULL) {
        p = strchr(p, ':');
        if (p) samples = (uint8_t)atoi(p + 1);
    }

    // Optional: power-monitor override (string field)
    // Accepts "auto" / "voltage" / "ina219" / "disabled"; ignored if absent or unknown.
    char power_override[16] = {0};
    if ((p = strstr(buf, "\"power_override\"")) != NULL) {
        char *q = strchr(p, ':');
        if (q) {
            char *start = strchr(q, '"');
            if (start) {
                start++;
                char *end = strchr(start, '"');
                if (end && (end - start) < (int)sizeof(power_override)) {
                    memcpy(power_override, start, end - start);
                    power_override[end - start] = '\0';
                }
            }
        }
    }

    // Clamp values
    if (sleep_s < 60)    sleep_s = 60;
    if (sleep_s > 86400) sleep_s = 86400;
    if (samples < 3)     samples = 3;
    if (samples > 20)    samples = 20;

    nvs_handle_t h;
    esp_err_t err = nvs_open("system", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS open failed");
        return ESP_FAIL;
    }
    nvs_set_u32(h, "sleep_s", sleep_s);
    nvs_set_u8(h, "samples", samples);
    nvs_commit(h);
    nvs_close(h);

    // Apply power-mode override (separate NVS namespace, validated by power_set_override)
    if (power_override[0] != '\0') {
        esp_err_t perr = power_set_override(power_override);
        if (perr != ESP_OK) {
            ESP_LOGW(TAG, "Ignoring invalid power_override='%s'", power_override);
        }
    }

    ESP_LOGI(TAG, "Config saved: sleep=%lus samples=%d power_override=%s",
             (unsigned long)sleep_s, (int)samples,
             power_override[0] ? power_override : "(unchanged)");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", -1);
}

// ── API: POST /api/ota ───────────────────────────────────────────────────────
static esp_err_t handle_ota(httpd_req_t *req) {
    touch_activity();
    ESP_LOGI(TAG, "OTA upload starting (%d bytes)", req->content_len);

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_h;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &ota_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char buf[1024];
    int total = 0;
    int remaining = req->content_len;

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, (remaining < sizeof(buf)) ? remaining : sizeof(buf));
        if (recv_len <= 0) {
            ESP_LOGE(TAG, "OTA recv failed at %d bytes", total);
            esp_ota_abort(ota_h);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload interrupted");
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_h, buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_h);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }

        total += recv_len;
        remaining -= recv_len;
        touch_activity();
    }

    err = esp_ota_end(ota_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Boot partition set failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA SUCCESS: %d bytes written. Rebooting in 2s...", total);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", 2);

    // Delay to let response reach the client, then reboot
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK; // never reached
}

// ── API: POST /api/reset ─────────────────────────────────────────────────────
static esp_err_t handle_reset(httpd_req_t *req) {
    touch_activity();
    ESP_LOGW(TAG, "FACTORY RESET requested via web UI");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", -1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    nvs_flash_erase();
    esp_restart();
    return ESP_OK;
}

// ── API: GET / (serve HTML page) ─────────────────────────────────────────────
static esp_err_t handle_root(httpd_req_t *req) {
    touch_activity();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE_HTML, -1);
}

// ── WiFi AP setup ────────────────────────────────────────────────────────────
static void wifi_ap_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .channel = 6,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    strncpy((char *)wifi_config.ap.ssid, s_ap_ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(s_ap_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Max TX power for better visibility on small C3 antenna
    esp_wifi_set_max_tx_power(80);  // 80 = 20dBm (max)

    ESP_LOGI(TAG, "WiFi AP started: SSID='%s' ch=6 IP=192.168.4.1", s_ap_ssid);
}

// ── HTTP server setup ────────────────────────────────────────────────────────
static httpd_handle_t start_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 5;
    config.recv_wait_timeout = 30;  // 30s for large uploads

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",           .method = HTTP_GET,  .handler = handle_root },
        { .uri = "/api/info",   .method = HTTP_GET,  .handler = handle_info },
        { .uri = "/api/config", .method = HTTP_POST, .handler = handle_config },
        { .uri = "/api/ota",    .method = HTTP_POST, .handler = handle_ota },
        { .uri = "/api/reset",  .method = HTTP_POST, .handler = handle_reset },
    };

    for (int i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }

    ESP_LOGI(TAG, "HTTP server started on port 80");
    return server;
}

// ── Public entry point ───────────────────────────────────────────────────────
void wifi_ota_start(int led_gpio, uint16_t device_addr) {
    s_led_gpio = led_gpio;
    if (device_addr > 0) {
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "TankSync-%d", (int)device_addr);
    }
    ESP_LOGI(TAG, "=== ENTERING WIFI OTA MODE ===");
    ESP_LOGI(TAG, "Connect to WiFi '%s' → open http://192.168.4.1", s_ap_ssid);

    wifi_ap_init();
    httpd_handle_t server = start_server();

    if (!server) {
        ESP_LOGE(TAG, "Server failed — rebooting");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    touch_activity();

    // Wait for activity or timeout
    while (1) {
        int64_t idle = esp_timer_get_time() - s_last_activity_us;
        if (idle > TIMEOUT_US) {
            ESP_LOGW(TAG, "5-minute timeout — rebooting to normal mode");
            break;
        }

        // Blink LED to show we're in update mode (fast double-blink)
        gpio_set_level(s_led_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(s_led_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(s_led_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(s_led_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(1700));
    }

    httpd_stop(server);
    esp_wifi_stop();
    esp_restart();
}
