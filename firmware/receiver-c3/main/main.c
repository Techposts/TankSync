// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

/**
 * TankSync Receiver v2 — app_main
 *
 * Boot sequence:
 *   1. NVS init
 *   2. Registry (SPIFFS + transmitter data model)
 *   3. WiFi (STA or AP fallback)
 *   4. Web server (always on — works in AP mode too)
 *   5. MQTT (only when WiFi connected)
 *   6. LoRa RX task (always listening)
 *   7. Display task (SH1106 OLED)
 *   8. LED task (WS2812B)
 *   9. OTA manager (24h auto-check)
 *
 * Inter-task communication: single EventGroupHandle_t (s_events)
 * Data store: transmitter_registry (mutex-protected, SPIFFS persistence)
 */

#include "config.h"
#include "transmitter_registry.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "ota_manager.h"
#include "web_server.h"
#include "lora_rylr998.h"
#include "display_sh1106.h"
#include "led_ws2812.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "main";

// ── System-wide event group (shared with all components) ──────────────────────
static EventGroupHandle_t s_events;

// ── LoRa OTA synchronisation primitives ──────────────────────────────────────
// Written by on_lora_raw_rx (lora_rx_task context), read by lora_ota_task.
static volatile uint32_t s_ota_ack_offset = 0;  // last confirmed TX offset
static SemaphoreHandle_t s_ota_ack_sem   = NULL; // given when OTA_ACK received
static SemaphoreHandle_t s_ota_ready_sem = NULL; // given when OTA_READY received
static volatile int64_t  s_ota_task_start_us = 0;    // timestamp guard — auto-expires
#define OTA_TASK_MAX_US  (120LL * 1000000LL)         // 2-minute max lifetime

// Forward declarations
static void lora_ota_task(void *arg);

// ── Raw LoRa RX callback — handles OTA_ACK / OTA_READY / SET_ACK ─────────────
static void on_lora_raw_rx(uint16_t src_addr, const char *payload,
                            int rssi, int snr) {
    (void)rssi; (void)snr;
    if (strncmp(payload, "OTA_ACK:", 8) == 0) {
        s_ota_ack_offset = (uint32_t)strtoul(payload + 8, NULL, 10);
        xSemaphoreGive(s_ota_ack_sem);
        ESP_LOGI(TAG, "OTA_ACK from %d: offset=%" PRIu32, src_addr, s_ota_ack_offset);
    } else if (strcmp(payload, "OTA_READY") == 0) {
        xSemaphoreGive(s_ota_ready_sem);
        ESP_LOGI(TAG, "OTA_READY from %d", src_addr);
    } else if (strcmp(payload, "OTA_DONE") == 0) {
        ESP_LOGI(TAG, "OTA_DONE from %d — transmitter rebooting", src_addr);
        registry_set_ota_progress(src_addr, 0, false); // mark complete
    } else if (strncmp(payload, "OTA_ERR:", 8) == 0) {
        ESP_LOGE(TAG, "OTA_ERR from %d: %s", src_addr, payload + 8);
        // Give ACK sem so lora_ota_task unblocks and sees offset mismatch
        xSemaphoreGive(s_ota_ack_sem);
    } else if (strcmp(payload, "SET_ACK") == 0) {
        ESP_LOGI(TAG, "Config acknowledged by %d — clearing pending flag", src_addr);
        registry_clear_pending_config(src_addr);
    } else {
        ESP_LOGD(TAG, "Raw from %d: %s", src_addr, payload);
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// LoRa RX callback — called from lora_rx_task when packet arrives
// ─────────────────────────────────────────────────────────────────────────────
static void on_lora_rx(const lora_rx_packet_t *pkt) {
    if (!pkt->data_valid) return;

    bool updated = registry_update_data(
        pkt->src_addr, pkt->raw_dist_cm,
        pkt->battery_pct, pkt->battery_voltage,
        pkt->msg_id, pkt->rssi, pkt->snr,
        pkt->fw_version);

    if (updated) {
        xEventGroupSetBits(s_events, EVT_NEW_LORA_DATA);
        // Publish to MQTT immediately
        int idx = registry_find(pkt->src_addr);
        if (idx >= 0) mqtt_publish_tank(idx);

        // CHECK FOR PENDING CONFIG DOWNLINK
        uint32_t sleep_s; uint8_t samples;
        if (registry_get_pending_config(pkt->src_addr, &sleep_s, &samples)) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "SET:SLEEP=%u:SAMP=%u", (unsigned)sleep_s, samples);
            ESP_LOGI(TAG, "Sending downlink config to %d: %s", pkt->src_addr, cmd);
            vTaskDelay(pdMS_TO_TICKS(500)); // Brief delay after ACK
            lora_send_async(pkt->src_addr, cmd);
        }

        // CHECK FOR PENDING LORA OTA
        bool ota_p = false; uint32_t ota_o = 0;
        if (registry_get_ota_status(pkt->src_addr, &ota_p, &ota_o) && ota_p && ota_o == 0) {
            bool ota_busy = s_ota_task_start_us > 0 &&
                (esp_timer_get_time() - s_ota_task_start_us) < OTA_TASK_MAX_US;
            if (ota_busy) {
                ESP_LOGW(TAG, "OTA task already running — skipping for %d", pkt->src_addr);
            } else {
                xSemaphoreTake(s_ota_ready_sem, 0);
                xSemaphoreTake(s_ota_ack_sem,   0);
                ESP_LOGI(TAG, "Triggering LoRa OTA task for %d", pkt->src_addr);
                xTaskCreate(lora_ota_task, "lo_ota", 4096, (void*)(uintptr_t)pkt->src_addr, 5, NULL);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Display helpers
// ─────────────────────────────────────────────────────────────────────────────

// 8×8 status icons — same bitmaps as Arduino version (row-major, MSB first)
static const uint8_t ICON_WIFI_ON[]  = {0x00,0x3C,0x42,0x18,0x24,0x00,0x18,0x00};
static const uint8_t ICON_WIFI_AP[]  = {0x00,0x3C,0x42,0x5A,0x24,0x18,0x18,0x00};
static const uint8_t ICON_WIFI_OFF[] = {0x00,0x3C,0x42,0x19,0x26,0x08,0x10,0x00};
static const uint8_t ICON_LORA_ON[]  = {0x18,0x18,0x18,0x3C,0x7E,0x18,0x18,0x18};

static void draw_bitmap(int x, int y, const uint8_t *bmp) {
    for (int row = 0; row < 8; row++) {
        uint8_t b = bmp[row];
        for (int col = 0; col < 8; col++) {
            if (b & (0x80 >> col)) disp_pixel(x + col, y + row, true);
        }
    }
}

// Navigation dots at y=60-63 (filled square = current, outline = other)
static void draw_nav_dots(int cur, int total) {
    if (total <= 1) return;
    int total_w = total * 6 - 2;
    int x = (128 - total_w) / 2;
    for (int i = 0; i < total; i++) {
        if (i == cur) disp_fill_rect(x, 60, 4, 4, true);
        else          disp_rect     (x, 60, 4, 4, true);
        x += 6;
    }
}

// Load screen enable bitmask from NVS (bit N = SCREEN_N enabled, default all on)
static uint8_t load_display_mask(void) {
    nvs_handle_t h;
    uint8_t mask = 0x1F;
    if (nvs_open("display", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "mask", &mask);
        nvs_close(h);
    }
    return mask ? mask : 0x1F;
}

static void build_screen_list(uint8_t mask, disp_screen_t *list, int *count) {
    *count = 0;
    for (int i = 0; i < SCREEN_COUNT; i++) {
        if (mask & (1 << i)) list[(*count)++] = (disp_screen_t)i;
    }
}

// ── Status bar (y=0..9): WiFi icon | [tank idx] | water% + mini tank ─────────
static void draw_status_bar(wifi_status_t ws, int tx_count, int tank_idx,
                             int water_pct, bool has_data) {
    const uint8_t *wifi_icon = (ws == WIFI_ST_CONNECTED) ? ICON_WIFI_ON :
                               (ws == WIFI_ST_AP_MODE)   ? ICON_WIFI_AP : ICON_WIFI_OFF;
    draw_bitmap(0, 1, wifi_icon);

    if (tx_count > 1) {
        char s[32];
        snprintf(s, sizeof(s), "[%d/%d]", tank_idx + 1, tx_count);
        disp_text(14, 1, FONT_SMALL, s);
    } else {
        draw_bitmap(44, 1, ICON_LORA_ON);
    }

    if (has_data) {
        char s[6];
        snprintf(s, sizeof(s), "%d%%", water_pct);
        disp_text(85, 1, FONT_SMALL, s);
        disp_tank_graphic(118, 1, 8, 8, water_pct);
    } else {
        disp_text(88, 1, FONT_SMALL, "---");
    }
    disp_hline(0, 10, 128, true);
}

// ── SCREEN_WATER: big tank graphic left, large % right (Arduino style) ────────
static void draw_water_screen(int tidx, int tx_count) {
    tx_info_t info; tx_data_t data;
    if (!registry_get_info(tidx, &info) || !registry_get_data(tidx, &data)) return;
    bool has = (data.state == TX_STATE_CONNECTED || data.state == TX_STATE_STALE);
    int  yo  = (tx_count > 1) ? 8 : 0;

    if (tx_count > 1) {
        char n[TX_NAME_MAX]; strncpy(n, info.name, 12); n[12] = 0;
        disp_text(0, 12, FONT_SMALL, n);
    }

    int graph_h = 36 - yo / 2;
    disp_tank_graphic(4, 14 + yo, 40, graph_h, has ? data.water_pct : 0);

    if (has) {
        char pct_str[4];
        snprintf(pct_str, sizeof(pct_str), "%d", data.water_pct);
        int digits = (int)strlen(pct_str);
        // Mirror Arduino cursor positions: 3-digit=x50, 2-digit=x56, 1-digit=x68
        int pct_x = (digits == 3) ? 50 : (digits == 2) ? 56 : 68;
        disp_text(pct_x, 15 + yo, FONT_LARGE, pct_str);
        disp_text(pct_x + digits * 18, 15 + yo, FONT_SMALL, "%");

        char liters[22];
        snprintf(liters, sizeof(liters), "%dL/%dL",
                 (int)data.water_liters, (int)info.capacity_liters);
        disp_text(50, 42 + yo, FONT_SMALL, liters);

        // Last updated timestamp
        if (data.last_update_us > 0) {
            int64_t age_s = (esp_timer_get_time() - data.last_update_us) / 1000000LL;
            char ago[16];
            if (age_s < 60)       snprintf(ago, sizeof(ago), "%ds ago", (int)age_s);
            else if (age_s < 3600) snprintf(ago, sizeof(ago), "%dm ago", (int)(age_s / 60));
            else                   snprintf(ago, sizeof(ago), "%dh ago", (int)(age_s / 3600));
            disp_text(50, 52 + yo, FONT_SMALL, ago);
        }
    } else {
        disp_text(56, 22 + yo, FONT_MEDIUM, "--");
        disp_text(50, 42 + yo, FONT_SMALL, "Waiting...");
    }
}

// ── SCREEN_BATTERY: battery outline + fill bar + %%  V text ──────────────────
static void draw_battery_screen(int tidx, int tx_count) {
    tx_info_t info; tx_data_t data;
    if (!registry_get_info(tidx, &info) || !registry_get_data(tidx, &data)) return;
    bool has = data.data_valid;
    int  yo  = (tx_count > 1) ? 6 : 0;

    if (tx_count > 1) {
        char n[TX_NAME_MAX]; strncpy(n, info.name, 12); n[12] = 0;
        disp_text(0, 12, FONT_SMALL, n);
    }

    int bat_x = 20, bat_y = 16 + yo, bat_w = 50, bat_h = 24;
    disp_battery_graphic(bat_x, bat_y, bat_w, bat_h, has ? data.battery_pct : 0);

    if (has) {
        char s[20];
        snprintf(s, sizeof(s), "%d%%  %.2fV", data.battery_pct, (double)data.battery_voltage);
        disp_text(bat_x, bat_y + bat_h + 5, FONT_SMALL, s);
    } else {
        disp_text(bat_x, bat_y + bat_h + 5, FONT_SMALL, "---% --V");
    }
}

// ── SCREEN_SIGNAL: RSSI large text + SNR ─────────────────────────────────────
static void draw_signal_screen(int tidx, int tx_count) {
    tx_info_t info; tx_data_t data;
    if (!registry_get_info(tidx, &info) || !registry_get_data(tidx, &data)) return;
    bool has = data.data_valid;
    int  yo  = (tx_count > 1) ? 6 : 0;

    if (tx_count > 1) {
        char n[TX_NAME_MAX]; strncpy(n, info.name, 12); n[12] = 0;
        disp_text(0, 12, FONT_SMALL, n);
    }

    if (has) {
        char s[14];
        snprintf(s, sizeof(s), "%ddBm", data.rssi);
        disp_text(10, 20 + yo, FONT_MEDIUM, s);
        snprintf(s, sizeof(s), "SNR: %d", data.snr);
        disp_text(10, 42 + yo, FONT_SMALL, s);
    } else {
        disp_text(10, 20 + yo, FONT_MEDIUM, "--");
        disp_text(10, 42 + yo, FONT_SMALL, "SNR: --");
    }
}

// ── SCREEN_DIAGNOSTICS: raw cm + state for all tanks (2-col when >3) ─────────
static void draw_diagnostics_screen(int tx_count) {
    disp_text(0, 12, FONT_SMALL, "Diagnostics");
    disp_hline(0, 21, 128, true);

    bool two_col = (tx_count > 3);
    int  row_h = 10, y0 = 23;

    for (int i = 0; i < tx_count && i < 8; i++) {
        tx_data_t d;
        if (!registry_get_data(i, &d)) continue;
        int x = two_col ? (i % 2) * 64 : 0;
        int y = y0 + (two_col ? (i / 2) : i) * row_h;
        if (y > 56) break;

        const char *st = (d.state == TX_STATE_CONNECTED) ? "OK"  :
                         (d.state == TX_STATE_STALE)     ? "STA" : "ERR";
        char line[22];
        if (d.data_valid)
            snprintf(line, sizeof(line), "T%d:%dcm %s", i + 1, d.raw_dist_cm, st);
        else
            snprintf(line, sizeof(line), "T%d:---- %s", i + 1, st);
        disp_text(x, y, FONT_SMALL, line);
    }
}

// ── SCREEN_SYSTEM: uptime, heap, FW version, IP, LoRa stats ──────────────────
static void draw_system_screen(wifi_status_t ws) {
    uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    char s[32];

    snprintf(s, sizeof(s), "Up:%uh%02um",
             (unsigned)(up_s / 3600), (unsigned)(up_s % 3600 / 60));
    disp_text(0, 12, FONT_SMALL, s);

    snprintf(s, sizeof(s), "Heap:%ldK", (long)(esp_get_free_heap_size() / 1024));
    disp_text(64, 12, FONT_SMALL, s);

    snprintf(s, sizeof(s), "FW: v%s", FIRMWARE_VERSION);
    disp_text(0, 22, FONT_SMALL, s);

    disp_text(0, 32, FONT_SMALL, wifi_manager_ip());

    int total_pkts = 0;
    for (int i = 0; i < registry_count(); i++) {
        tx_data_t d; if (registry_get_data(i, &d)) total_pkts += (int)d.packets_rx;
    }
    snprintf(s, sizeof(s), "LoRa: %d pkts", total_pkts);
    disp_text(0, 42, FONT_SMALL, s);

    if (ws == WIFI_ST_CONNECTED) {
        snprintf(s, sizeof(s), "WiFi: %ddBm", wifi_manager_rssi());
        disp_text(0, 52, FONT_SMALL, s);
    } else if (ws == WIFI_ST_AP_MODE) {
        disp_text(0, 52, FONT_SMALL, "WiFi: AP mode");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Display task — rotates through enabled screens every DISPLAY_SCREEN_MS
// Per-tank screens (WATER/BATTERY/SIGNAL) show current_tank.
// Tank index advances each full screen cycle.
// Screen enable mask loaded from NVS ("display"/"mask"); web UI can change it.
// ─────────────────────────────────────────────────────────────────────────────
static void display_task(void *arg) {
    esp_err_t disp_err = disp_init(PIN_I2C_SDA, PIN_I2C_SCL, DISPLAY_I2C_ADDR);
    if (disp_err != ESP_OK) {
        ESP_LOGW(TAG, "Display init failed — display task exiting");
        vTaskDelete(NULL);
        return;
    }

    uint8_t       scr_mask  = load_display_mask();
    disp_screen_t scr_list[SCREEN_COUNT];
    int           scr_count = 0;
    build_screen_list(scr_mask, scr_list, &scr_count);

    int        scr_idx  = 0;   // index into scr_list[]
    int        tank_idx = 0;   // which tank for per-tank screens
    TickType_t last_sw  = xTaskGetTickCount();

    for (;;) {
        registry_update_states();

        int tx_count = registry_count();
        wifi_status_t ws = wifi_manager_status();

        TickType_t now = xTaskGetTickCount();
        if ((now - last_sw) >= pdMS_TO_TICKS(DISPLAY_SCREEN_MS)) {
            last_sw = now;
            int prev = scr_idx;
            scr_idx = (scr_idx + 1) % scr_count;
            // Wrapped back to first screen → advance tank
            if (scr_idx <= prev && tx_count > 0)
                tank_idx = (tank_idx + 1) % tx_count;

            // Reload mask so web UI changes take effect on next cycle
            scr_mask = load_display_mask();
            build_screen_list(scr_mask, scr_list, &scr_count);
            if (scr_count == 0) { scr_mask = 0x1F; build_screen_list(scr_mask, scr_list, &scr_count); }
            if (scr_idx >= scr_count) scr_idx = 0;
        }

        if (tx_count > 0 && tank_idx >= tx_count) tank_idx = 0;

        // Current tank data for status bar
        int  cur_pct  = 0;
        bool cur_data = false;
        if (tx_count > 0) {
            tx_data_t d;
            int t = (tank_idx < tx_count) ? tank_idx : 0;
            if (registry_get_data(t, &d) && d.data_valid) {
                cur_pct  = d.water_pct;
                cur_data = true;
            }
        }

        disp_screen_t cur_scr = (scr_count > 0) ? scr_list[scr_idx] : SCREEN_SYSTEM;

        disp_clear();

        if (lora_is_pairing_mode()) {
            disp_text(25, 5, FONT_MEDIUM, "PAIRING");
            disp_text(10, 25, FONT_SMALL, "Hold Button on TX");
            disp_text(30, 40, FONT_SMALL, "for 3 seconds");
            // Simple animated dots
            int dots = (xTaskGetTickCount() / pdMS_TO_TICKS(500)) % 4;
            char ds[5] = {0}; for(int i=0; i<dots; i++) ds[i]='.';
            disp_text(55, 55, FONT_SMALL, ds);
        } else {
            draw_status_bar(ws, tx_count, tank_idx, cur_pct, cur_data);
            draw_nav_dots(scr_idx, scr_count);

            switch (cur_scr) {
                case SCREEN_WATER:
                    if (tx_count > 0) draw_water_screen(tank_idx, tx_count);
                    else disp_text(10, 32, FONT_SMALL, "No transmitters");
                    break;
                case SCREEN_BATTERY:
                    if (tx_count > 0) draw_battery_screen(tank_idx, tx_count);
                    else disp_text(10, 32, FONT_SMALL, "No transmitters");
                    break;
                case SCREEN_SIGNAL:
                    if (tx_count > 0) draw_signal_screen(tank_idx, tx_count);
                    else disp_text(10, 32, FONT_SMALL, "No signal data");
                    break;
                case SCREEN_DIAGNOSTICS:
                    draw_diagnostics_screen(tx_count);
                    break;
                case SCREEN_SYSTEM:
                    draw_system_screen(ws);
                    break;
                default:
                    break;
            }
        }

        // Pause display during pairing and for 5s after — I2C + LoRa
        // address change on ESP32-C3 can lock the APB bus during I2C recovery.
        // The holdoff after pairing lets the UART settle before I2C resumes.
        {
            static int64_t s_pair_end_us = 0;
            if (lora_is_pairing_mode()) {
                s_pair_end_us = esp_timer_get_time();
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            if (s_pair_end_us > 0 &&
                (esp_timer_get_time() - s_pair_end_us) < 5000000LL) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            s_pair_end_us = 0;
        }

        esp_err_t flush_err = disp_flush();
        if (flush_err != ESP_OK) {
            ESP_LOGW(TAG, "Display I2C failed (%s) — suspending display task",
                     esp_err_to_name(flush_err));
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_UPDATE_MS));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// LED task — reflects system state
// ─────────────────────────────────────────────────────────────────────────────
static void led_task(void *arg) {
    led_init(PIN_LED_DATA, LED_COUNT, LED_BRIGHTNESS);

    TickType_t  blink_tick = xTaskGetTickCount();
    bool        blink_on   = true;
    uint32_t    data_flash = 0;  // ms remaining for data flash

    for (;;) {
        wifi_status_t ws = wifi_manager_status();

        // Status LED (index 0) — WiFi/MQTT state
        led_color_t status_col;
        if (ws == WIFI_ST_CONNECTED &&
            mqtt_manager_status() == MQTT_ST_CONNECTED) {
            status_col = LED_GREEN;                     // WiFi + MQTT
        } else if (ws == WIFI_ST_CONNECTED) {
            status_col = LED_CYAN;                      // WiFi only
        } else if (ws == WIFI_ST_AP_MODE) {
            // Slow blue blink for AP mode
            if ((xTaskGetTickCount() - blink_tick) >= pdMS_TO_TICKS(1000)) {
                blink_tick = xTaskGetTickCount();
                blink_on = !blink_on;
            }
            status_col = blink_on ? LED_BLUE : LED_OFF;
        } else if (ws == WIFI_ST_CONNECTING) {
            // Fast blink yellow
            if ((xTaskGetTickCount() - blink_tick) >= pdMS_TO_TICKS(250)) {
                blink_tick = xTaskGetTickCount();
                blink_on = !blink_on;
            }
            status_col = blink_on ? LED_YELLOW : LED_OFF;
        } else {
            status_col = LED_RED;
        }

        // Water LED (index 1) — worst tank state
        tx_state_t worst = registry_worst_state();
        int online = registry_online_count();
        led_color_t water_col;
        if (online == 0) {
            water_col = LED_OFF;
        } else {
            // Get lowest water level across all online tanks
            int min_pct = 100;
            for (int i = 0; i < registry_count(); i++) {
                tx_info_t info; tx_data_t data;
                if (!registry_get_info(i, &info) || !info.enabled) continue;
                if (!registry_get_data(i, &data) || data.state != TX_STATE_CONNECTED) continue;
                if (data.water_pct < min_pct) min_pct = data.water_pct;
            }
            if (worst == TX_STATE_LOST || worst == TX_STATE_WAITING) {
                water_col = LED_WHITE;              // unknown / offline
            } else if (min_pct > 50) {
                water_col = LED_GREEN;
            } else if (min_pct > 20) {
                water_col = LED_YELLOW;
            } else {
                water_col = LED_RED;
            }
        }

        // Flash white briefly when new LoRa data arrives
        EventBits_t bits = xEventGroupWaitBits(s_events, EVT_NEW_LORA_DATA,
                                               pdTRUE, pdFALSE, 0);
        if (bits & EVT_NEW_LORA_DATA) data_flash = 300;
        if (data_flash > 0) {
            status_col = LED_WHITE;
            data_flash = (data_flash > 100) ? data_flash - 100 : 0;
        }

        // OTA in progress — both LEDs cycle
        if (xEventGroupGetBits(s_events) & EVT_OTA_IN_PROGRESS) {
            if ((xTaskGetTickCount() - blink_tick) >= pdMS_TO_TICKS(200)) {
                blink_tick = xTaskGetTickCount();
                blink_on = !blink_on;
            }
            status_col = blink_on ? LED_ORANGE : LED_OFF;
            water_col  = blink_on ? LED_OFF    : LED_ORANGE;
        }

        led_set(LED_IDX_STATUS, status_col);
        led_set(LED_IDX_WATER,  water_col);
        led_show();

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// WiFi event watcher — starts MQTT and OTA when WiFi connects
// ─────────────────────────────────────────────────────────────────────────────
static void wifi_watch_task(void *arg) {
    bool mqtt_started = false;
    bool ota_started  = false;

    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_events,
            EVT_WIFI_CONNECTED | EVT_WIFI_DISCONNECTED,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));

        if (bits & EVT_WIFI_CONNECTED) {
            if (!mqtt_started) {
                mqtt_manager_start();
                mqtt_started = true;
            }
            if (!ota_started) {
                // OTA task already running; just let it do its first check
                ota_started = true;
            }
            xEventGroupClearBits(s_events, EVT_WIFI_CONNECTED);
        }
        if (bits & EVT_WIFI_DISCONNECTED) {
            // MQTT will handle reconnect internally (esp_mqtt_client auto-reconnects)
            xEventGroupClearBits(s_events, EVT_WIFI_DISCONNECTED);
        }

        // Periodic MQTT system publish every 5 min
        static TickType_t last_sys_publish = 0;
        if ((xTaskGetTickCount() - last_sys_publish) >= pdMS_TO_TICKS(300000)) {
            last_sys_publish = xTaskGetTickCount();
            mqtt_publish_system();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// app_main
// ─────────────────────────────────────────────────────────────────────────────
void app_main(void) {
    ESP_LOGI(TAG, "TankSync Receiver v%s booting...", FIRMWARE_VERSION);

    // ── 1. NVS ──
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS full/version mismatch — erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // ── 2. Event group ──
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(s_events ? ESP_OK : ESP_ERR_NO_MEM);

    // ── 3. Transmitter registry (SPIFFS) ──
    ESP_ERROR_CHECK(registry_init());

    // ── 4. WiFi ──
    ESP_ERROR_CHECK(wifi_manager_init(s_events));
    wifi_manager_connect();  // STA if creds saved, AP fallback

    // ── 5. MQTT init (doesn't connect yet) ──
    ESP_ERROR_CHECK(mqtt_manager_init());

    // ── 6. Web server (works in both STA and AP mode) ──
    ESP_ERROR_CHECK(web_server_start());

    // ── 7. LoRa ──
    // Create OTA sync primitives before lora_init so callbacks are ready
    s_ota_ack_sem   = xSemaphoreCreateBinary();
    s_ota_ready_sem = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK((s_ota_ack_sem && s_ota_ready_sem) ? ESP_OK : ESP_ERR_NO_MEM);

    esp_err_t lora_err = lora_init(LORA_UART_NUM, PIN_LORA_TX, PIN_LORA_RX, LORA_BAUD);
    if (lora_err != ESP_OK) {
        ESP_LOGW(TAG, "LoRa init failed — continuing without LoRa (web UI still available)");
    }
    lora_set_rx_callback(on_lora_rx);
    lora_set_raw_rx_callback(on_lora_raw_rx);
    // LoRa RX task is spawned inside lora_init

    // ── 8. OTA manager (marks current firmware valid, starts 24h check loop) ──
    ESP_ERROR_CHECK(ota_manager_init());

    // ── 9. Application tasks ──
    xTaskCreate(display_task,   "display",    STACK_DISPLAY, NULL, TASK_PRIO_DISPLAY, NULL);
    xTaskCreate(led_task,       "led",        STACK_LED,     NULL, TASK_PRIO_LED,     NULL);
    xTaskCreate(wifi_watch_task,"wifi_watch", 3072,          NULL, TASK_PRIO_WIFI,    NULL);

    ESP_LOGI(TAG, "All tasks started. Free heap: %lu bytes",
             (unsigned long)esp_get_free_heap_size());

    // app_main can now exit (or loop here for monitoring)
    // Deleting ourselves frees the 8KB app_main stack
    vTaskDelete(NULL);
}

// ── LoRa OTA Task (Receiver) ─────────────────────────────────────────────────
//
// Protocol:
//   RX → TX  OTA_START:<total_bytes>     (transmitter wakes OTA handler)
//   TX → RX  OTA_READY                   (transmitter ready, s_ota_ready_sem given)
//   RX → TX  OTA_DATA:<offset>:<hexdata> (100-byte chunk = 200 hex chars, ≤219 bytes total)
//   TX → RX  OTA_ACK:<next_offset>       (per-chunk, s_ota_ack_sem given)
//   RX → TX  OTA_END                     (all chunks sent)
//   TX → RX  OTA_DONE                    (transmitter verified + rebooting)
//
// Timeouts: 8 s for OTA_READY, 8 s per chunk ACK.  3 retries per chunk.
// Chunk size: 100 bytes → 200 hex chars.  OTA_DATA:4294967295: = 19 chars.
//   Total = 219 bytes < 240-byte RYLR998 payload limit.
//
#define OTA_CHUNK_BYTES   100
#define OTA_READY_TIMEOUT pdMS_TO_TICKS(8000)
#define OTA_ACK_TIMEOUT   pdMS_TO_TICKS(2000)
#define OTA_MAX_RETRIES   10

static void lora_ota_task(void *arg) {
    s_ota_task_start_us = esp_timer_get_time();
    uint16_t addr = (uint16_t)(uintptr_t)arg;
    ESP_LOGI(TAG, "LoRa OTA start → addr %d", addr);

    FILE *f = fopen("/spiffs/tx_fw.bin", "rb");
    if (!f) {
        ESP_LOGE(TAG, "No staged firmware at /spiffs/tx_fw.bin");
        registry_set_ota_progress(addr, 0, false);
        s_ota_task_start_us = 0;
        vTaskDelete(NULL);
        return;
    }

    fseek(f, 0, SEEK_END);
    uint32_t total_size = (uint32_t)ftell(f);
    rewind(f);
    ESP_LOGI(TAG, "Firmware size: %" PRIu32 " bytes", total_size);

    // ── Version check: skip OTA if TX already runs the staged firmware ──────
    char staged_ver[32] = {0};
    {
        uint8_t scan[256];
        size_t scan_off = 0;
        bool found = false;
        rewind(f);
        while (scan_off < 4096 && !found) {
            size_t n = fread(scan, 1, sizeof(scan), f);
            if (n < 4) break;
            for (size_t i = 0; i + 48 <= n; i += 4) {
                uint32_t w = (uint32_t)scan[i] | ((uint32_t)scan[i+1]<<8)
                           | ((uint32_t)scan[i+2]<<16) | ((uint32_t)scan[i+3]<<24);
                if (w == 0xABCD5432u) {
                    memcpy(staged_ver, &scan[i + 16], 31);
                    staged_ver[31] = '\0';
                    found = true;
                    break;
                }
            }
            if (!found && n == sizeof(scan)) {
                fseek(f, -(long)48, SEEK_CUR);
                scan_off += sizeof(scan) - 48;
            } else if (!found) break;
        }
        rewind(f);
    }
    if (staged_ver[0]) {
        int idx = registry_find(addr);
        if (idx >= 0) {
            tx_data_t data;
            if (registry_get_data(idx, &data) && data.fw_version[0] &&
                strcmp(data.fw_version, staged_ver) == 0) {
                ESP_LOGI(TAG, "TX %d already running v%s — skipping OTA", addr, staged_ver);
                fclose(f);
                registry_set_ota_progress(addr, 0, false);
                s_ota_task_start_us = 0;
                vTaskDelete(NULL);
                return;
            }
        }
        ESP_LOGI(TAG, "Staged firmware: v%s", staged_ver);
    }

    // ── Step 0: Send OTA_START to TX (at normal SF9/125kHz) ─────────────────
    char ota_start_cmd[48];
    snprintf(ota_start_cmd, sizeof(ota_start_cmd), "OTA_START:%" PRIu32, total_size);
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Sending OTA_START to %d (%" PRIu32 " bytes) 3x", addr, total_size);
    for (int r = 0; r < 3; r++) {
        lora_send(addr, ota_start_cmd);
        vTaskDelay(pdMS_TO_TICKS(600));
    }

    ESP_LOGI(TAG, "Switching to SF7/500kHz for OTA chunks");
    lora_send_cmd("AT+PARAMETER=7,9,1,12", 1500);

    // ── Step 1: Wait for TX to be ready ─────────────────────────────────────
    ESP_LOGI(TAG, "Waiting up to 8s for OTA_READY from %d...", addr);
    if (xSemaphoreTake(s_ota_ready_sem, pdMS_TO_TICKS(8000)) == pdTRUE) {
        ESP_LOGI(TAG, "OTA_READY received — proceeding");
    } else {
        ESP_LOGW(TAG, "OTA_READY not received — proceeding anyway (TX may be ready)");
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    // ── Step 2: stream chunks ─────────────────────────────────────────────────
    char cmd[256];
    uint32_t offset = 0;
    uint8_t  chunk[OTA_CHUNK_BYTES];
    // hex buf: 2 chars/byte + null
    char hex[OTA_CHUNK_BYTES * 2 + 1];

    bool failed = false;
    while (offset < total_size) {
        size_t n = fread(chunk, 1, OTA_CHUNK_BYTES, f);
        if (n == 0) break;

        // Hex-encode chunk
        for (size_t i = 0; i < n; i++) {
            sprintf(hex + i * 2, "%02x", chunk[i]);
        }
        hex[n * 2] = '\0';

        snprintf(cmd, sizeof(cmd), "OTA_DATA:%" PRIu32 ":%s", offset, hex);

        bool acked = false;
        for (int retry = 0; retry < OTA_MAX_RETRIES; retry++) {
            if (retry > 0) {
                ESP_LOGW(TAG, "Retry %d for offset %" PRIu32, retry, offset);
            }

            // Drain stale ACK before sending
            xSemaphoreTake(s_ota_ack_sem, 0);

            // Send chunk directly from OTA task (lora_send uses mutex).
            // In earlier tests, direct lora_send DID deliver chunks to TX
            // (saw OTA_ACK:103), while lora_send_via_rx did not.
            bool chunk_sent = lora_send(addr, cmd);
            if (!chunk_sent) {
                ESP_LOGE(TAG, "lora_send FAIL for chunk at offset %" PRIu32, offset);
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            ESP_LOGI(TAG, "OTA chunk %" PRIu32 "/%" PRIu32 " sent OK, waiting ACK",
                     offset, total_size);

            if (xSemaphoreTake(s_ota_ack_sem, OTA_ACK_TIMEOUT) != pdTRUE) {
                ESP_LOGW(TAG, "ACK timeout at offset %" PRIu32, offset);
                continue;
            }

            uint32_t expected_next = offset + (uint32_t)n;
            if (s_ota_ack_offset == expected_next) {
                acked = true;
                break;
            }
            ESP_LOGW(TAG, "ACK offset mismatch: got %" PRIu32 " expected %" PRIu32,
                     s_ota_ack_offset, expected_next);
        }

        if (!acked) {
            ESP_LOGE(TAG, "OTA failed at offset %" PRIu32 " after %d retries",
                     offset, OTA_MAX_RETRIES);
            lora_send(addr, "OTA_ABORT");
            failed = true;
            break;
        }

        offset += (uint32_t)n;
        registry_set_ota_progress(addr, offset, true); // RAM update only

        // Persist to SPIFFS every 50 chunks (~5KB) to reduce flash wear
        // (down from every chunk — saves ~98% of SPIFFS writes)
        uint32_t chunk_num = offset / OTA_CHUNK_BYTES;
        if (chunk_num % 50 == 0 || offset >= total_size) {
            registry_persist();
        }

        // Log progress every ~10 chunks to avoid flooding logs
        if (chunk_num % 10 == 0 || offset >= total_size) {
            ESP_LOGI(TAG, "OTA progress: %" PRIu32 "/%" PRIu32 " (%.1f%%)",
                     offset, total_size, 100.0f * offset / total_size);
        }
    }

    fclose(f);

    if (!failed) {
        lora_send(addr, "OTA_END");
        ESP_LOGI(TAG, "OTA_END sent — waiting for OTA_DONE (still at SF7)");

        bool got_done = false;
        for (int i = 0; i < 10; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            bool ota_p = false; uint32_t ota_o = 0;
            registry_get_ota_status(addr, &ota_p, &ota_o);
            if (!ota_p) {
                ESP_LOGI(TAG, "OTA_DONE received from %d — OTA complete", addr);
                got_done = true;
                break;
            }
        }
        if (!got_done) {
            ESP_LOGW(TAG, "OTA_DONE not received after 10s — assuming success (TX rebooted)");
            registry_set_ota_progress(addr, 0, false);
        }
    } else {
        registry_set_ota_progress(addr, 0, false);
    }

    // Restore normal LoRa params (SF9/125kHz) for regular TANK reception
    ESP_LOGI(TAG, "Restoring SF9/125kHz params");
    lora_send_cmd("AT+PARAMETER=9,7,1,12", 1500);

    s_ota_task_start_us = 0;
    ESP_LOGI(TAG, "LoRa OTA task exit (addr=%d, success=%s)",
             addr, failed ? "no" : "yes");
    vTaskDelete(NULL);
}
