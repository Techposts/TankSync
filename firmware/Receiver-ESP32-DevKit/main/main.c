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
#include "geo_time.h"
#include "web_server.h"
#include "lora_rylr998.h"
#include "display_sh1106.h"
#include "led_ws2812.h"
#include "buzzer.h"
#include "log_buffer.h"

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

// Build provenance fingerprint — preserved across releases for audit.
// Referenced in app_main below so the linker keeps it in .rodata.
static const uint32_t TS_BUILD_FP = 0xA3F1D582u;

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
static void history_timer_cb(void *arg);

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


// ─── Per-TX buzzer alert state tracker ───────────────────────────────────────
// Tracks the last tank-tier alert we played for each TX so we can:
//   (a) fire transition alerts only on tier change, not on every packet
//   (b) re-trigger sustained alerts at the design cadence (60s critical, 30s
//       overflow, 5min low warning, 10min sensor offline) via history_timer_cb
// State is keyed by registry index. registry indices are stable for a TX's
// lifetime, so we don't need to hash by address.
typedef enum {
    ALERT_TIER_NONE      = 0,
    ALERT_TIER_LOW       = 1,   // <20% but ≥5%
    ALERT_TIER_CRITICAL  = 2,   // <5%
    ALERT_TIER_OVERFLOW  = 3,   // >95%
} buzz_tier_t;

#define BUZZ_MAX_TANKS 16
// TX battery alert fires once when crossing below this threshold. 20% leaves
// the user a few days to swap/charge before the TX dies. We do NOT recur — a
// running pump or empty tank is fix-it-now; a low TX battery is fix-it-soon.
#define BUZZ_TX_BAT_LOW_PCT 20
static struct {
    buzz_tier_t tier;
    int64_t     last_repeat_us;   // 0 = never repeated since entering tier
    bool        sensor_alert_active;
    int64_t     sensor_alert_last_us;
    bool        bat_low_active;   // armed once seen above threshold; fires once on cross-below
    bool        bat_seen_high;    // suppresses first-boot beep before any high reading exists
} s_buzz_per_tank[BUZZ_MAX_TANKS] = {0};

static buzz_tier_t classify_tier(int water_pct) {
    if (water_pct >  95) return ALERT_TIER_OVERFLOW;
    if (water_pct <   5) return ALERT_TIER_CRITICAL;
    if (water_pct <  20) return ALERT_TIER_LOW;
    return ALERT_TIER_NONE;
}

// Called from on_lora_rx after registry update — fires one-shot transition alerts.
static void buzz_check_transition(int idx, int water_pct, bool sensor_error, int battery_pct) {
    if (idx < 0 || idx >= BUZZ_MAX_TANKS) return;
    buzz_tier_t cur = classify_tier(water_pct);
    if (cur != s_buzz_per_tank[idx].tier) {
        s_buzz_per_tank[idx].tier = cur;
        s_buzz_per_tank[idx].last_repeat_us = esp_timer_get_time();
        switch (cur) {
            case ALERT_TIER_CRITICAL: buzzer_play(BUZZ_CRITICAL_LOW); break;
            case ALERT_TIER_OVERFLOW: buzzer_play(BUZZ_OVERFLOW);     break;
            case ALERT_TIER_LOW:      buzzer_play(BUZZ_LOW_WARNING);  break;
            default: /* recovered to NONE — no celebratory beep */    break;
        }
    }
    // Edge-triggered sensor-error alert.
    if (sensor_error && !s_buzz_per_tank[idx].sensor_alert_active) {
        s_buzz_per_tank[idx].sensor_alert_active  = true;
        s_buzz_per_tank[idx].sensor_alert_last_us = esp_timer_get_time();
        buzzer_play(BUZZ_SENSOR_OFFLINE);
    } else if (!sensor_error) {
        s_buzz_per_tank[idx].sensor_alert_active = false;
    }
    // Edge-triggered TX low-battery alert. Requires having seen the battery
    // above threshold at least once first — otherwise a brand-new TX paired
    // at <20% would beep on the very first packet, which is noise.
    if (battery_pct >= BUZZ_TX_BAT_LOW_PCT) {
        s_buzz_per_tank[idx].bat_seen_high = true;
        s_buzz_per_tank[idx].bat_low_active = false;
    } else if (battery_pct > 0 && battery_pct < BUZZ_TX_BAT_LOW_PCT
               && s_buzz_per_tank[idx].bat_seen_high
               && !s_buzz_per_tank[idx].bat_low_active) {
        s_buzz_per_tank[idx].bat_low_active = true;
        buzzer_play(BUZZ_TX_LOW_BATTERY);
    }
}

// Called from history_timer_cb (every 60s) — re-fires sustained alerts.
static void buzz_check_repeats(void) {
    // Only fix-it-now conditions get recurring repeats. Overflow (>95%) and
    // low warning (<20%) are edge-triggered only via buzz_check_transition —
    // a full tank sitting at 100% after refill is the desired state, not a
    // crisis to nag the user about every 30s.
    int64_t now = esp_timer_get_time();
    int tx_count = registry_count();
    if (tx_count > BUZZ_MAX_TANKS) tx_count = BUZZ_MAX_TANKS;
    for (int i = 0; i < tx_count; i++) {
        tx_data_t d;
        if (!registry_get_data(i, &d)) continue;
        // Bug fix (rx-v2.8.4): only alert on tanks we have CURRENT data for.
        // A never-reported tank has water_pct=0 in memory from registry init,
        // which classify_tier() reads as CRITICAL — would fire false-positive
        // Critical-low beeps every 60s for any unpaired-but-registered slot.
        // Stale/lost tanks also shouldn't keep nagging — the sensor-offline
        // alert covers that case at a different cadence.
        if (!d.data_valid || d.state != TX_STATE_CONNECTED) continue;
        buzz_tier_t cur = classify_tier(d.water_pct);
        int64_t since = now - s_buzz_per_tank[i].last_repeat_us;
        if (cur == ALERT_TIER_CRITICAL && since >= 60LL * 1000000LL) {
            buzzer_play(BUZZ_CRITICAL_LOW);
            s_buzz_per_tank[i].last_repeat_us = now;
        }
        // Overflow gets a gentle 1-hour re-nag — no on-screen escalation
        // tier exists above 100% (vs critical-low for low-water), so without
        // a slow re-nag a missed first alert means an actively-overflowing
        // pump wastes water + electricity unnoticed for hours.
        if (cur == ALERT_TIER_OVERFLOW && since >= 3600LL * 1000000LL) {
            buzzer_play(BUZZ_OVERFLOW);
            s_buzz_per_tank[i].last_repeat_us = now;
        }
        if (s_buzz_per_tank[i].sensor_alert_active) {
            int64_t since_s = now - s_buzz_per_tank[i].sensor_alert_last_us;
            if (since_s >= 600LL * 1000000LL) {
                buzzer_play(BUZZ_SENSOR_OFFLINE);
                s_buzz_per_tank[i].sensor_alert_last_us = now;
            }
        }
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
        pkt->fw_version,
        pkt->power_mode, pkt->current_ma, pkt->power_mw,
        pkt->sensor_status,
        pkt->sensor_kind);

    if (updated) {
        xEventGroupSetBits(s_events, EVT_NEW_LORA_DATA);
        // Publish to MQTT immediately
        int idx = registry_find(pkt->src_addr);
        if (idx >= 0) mqtt_publish_tank(idx);

        // Buzzer alerts — transition-only (re-trigger cadence is handled in
        // history_timer_cb at 60s tick). Fetches current data to read the
        // just-applied water_pct + sensor_error.
        if (idx >= 0) {
            tx_data_t d;
            if (registry_get_data(idx, &d)) {
                buzz_check_transition(idx, d.water_pct, d.sensor_error, d.battery_pct);
            }
        }

        // CHECK FOR PENDING CONFIG DOWNLINK
        uint32_t sleep_s; uint8_t samples; uint8_t pwr;
        char sensor_kind[12] = {0};
        if (registry_get_pending_config(pkt->src_addr, &sleep_s, &samples, &pwr,
                                        sensor_kind, sizeof(sensor_kind))) {
            // Build SET frame incrementally. Sleep + samples are always present;
            // PWR and SENSOR are appended only when non-default so legacy TX
            // firmwares (pre-2.0.15) safely ignore the trailing tokens.
            char cmd[96];
            int  n = snprintf(cmd, sizeof(cmd), "SET:SLEEP=%u:SAMP=%u",
                              (unsigned)sleep_s, samples);
            if (pwr > 0 && n < (int)sizeof(cmd)) {
                n += snprintf(cmd + n, sizeof(cmd) - n, ":PWR=%u", pwr);
            }
            if (sensor_kind[0] && n < (int)sizeof(cmd)) {
                n += snprintf(cmd + n, sizeof(cmd) - n, ":SENSOR=%s", sensor_kind);
            }
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
                // Drain stale semaphores before spawning OTA task
                xSemaphoreTake(s_ota_ready_sem, 0);
                xSemaphoreTake(s_ota_ack_sem,   0);
                // Spawn OTA task — it handles OTA_START, param switch, and chunks.
                // This avoids blocking lora_rx_task for 5+ seconds during OTA_START
                // sends, which would cause UART buffer overflow and lost OTA_READY.
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
// ICON_LORA_ON removed 2026-04-28 — slot in the status bar repurposed for the
// "N tanks" text since the new SCREEN_WATER list view shows all tanks at once.

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

// ── Top WiFi/system status bar (y=0..9) ──────────────────────────────────────
// Compact strip showing connectivity status only. Multi-tank water-level
// at-a-glance is handled by the SCREEN_WATER list view itself, so this strip
// no longer needs to carry per-tank info.
static void draw_status_bar(wifi_status_t ws, int tx_count) {
    const uint8_t *wifi_icon = (ws == WIFI_ST_CONNECTED) ? ICON_WIFI_ON :
                               (ws == WIFI_ST_AP_MODE)   ? ICON_WIFI_AP : ICON_WIFI_OFF;
    draw_bitmap(0, 1, wifi_icon);

    char s[24];
    snprintf(s, sizeof(s), "%d %s", tx_count, tx_count == 1 ? "tank" : "tanks");
    disp_text(14, 1, FONT_SMALL, s);

    if (ws == WIFI_ST_CONNECTED) {
        snprintf(s, sizeof(s), "%ddBm", wifi_manager_rssi());
        int w = disp_text_width(FONT_SMALL, s);
        disp_text(128 - w, 1, FONT_SMALL, s);
    }
    disp_hline(0, 10, 128, true);
}

// ── Helper: render a single tank row in the list view ────────────────────────
// Layout per row (h px tall):
//   y=0..(h-1):  name (FONT_SMALL) at x=0, % text right-aligned at x=128
//   bar at bottom of row (full width)
// LOW (<20% + valid data): row gets a left-edge attention indicator (3px wide
//   filled stripe) — visible from across the room as a black-on-white "tab".
// OFFLINE/WAITING: bar is outline-only; "—" / "..." replaces % number.
static void draw_tank_row(int idx, int y, int h, bool is_focused) {
    tx_info_t info; tx_data_t data;
    if (!registry_get_info(idx, &info)) return;
    bool has_data = registry_get_data(idx, &data) && data.data_valid;
    bool stale    = has_data && (data.state == TX_STATE_STALE || data.state == TX_STATE_LOST);
    bool waiting  = !has_data || data.state == TX_STATE_WAITING;
    bool low      = has_data && !stale && !waiting && data.water_pct < 20;

    const int bar_h = 4;
    const int text_y = y;
    const int bar_y = y + h - bar_h - 1;

    // Left attention stripe for LOW state (3 px wide × full row height) —
    // single strongest-contrast cue we can offer in 1-bit. Glanceable at 6 ft.
    int x0 = 0;
    if (low) {
        disp_fill_rect(0, y, 3, h, true);
        x0 = 5;
    }

    // Focus marker for the "current" tank in the rotation — small caret on
    // right edge. Subtle so it doesn't compete with LOW for attention.
    if (is_focused) {
        disp_pixel(127, y + 2, true);
        disp_pixel(127, y + 3, true);
        disp_pixel(127, y + 4, true);
        disp_pixel(126, y + 3, true);
    }

    // Name (truncated to 10 chars max — fits in FONT_SMALL × 60 px)
    char n[11] = {0};
    int name_len = (int)strlen(info.name);
    int copy = name_len < 10 ? name_len : 10;
    memcpy(n, info.name, copy);
    disp_text(x0, text_y, FONT_SMALL, n);

    // Right-aligned % (or status placeholder)
    char pct_str[8];
    if (waiting)      snprintf(pct_str, sizeof(pct_str), "...");
    else if (stale)   snprintf(pct_str, sizeof(pct_str), "OFF");
    else if (low)     snprintf(pct_str, sizeof(pct_str), "%d%%!", data.water_pct);
    else              snprintf(pct_str, sizeof(pct_str), "%d%%", data.water_pct);
    int pct_w = disp_text_width(FONT_SMALL, pct_str);
    int pct_x = 128 - pct_w - (is_focused ? 4 : 1);
    disp_text(pct_x, text_y, FONT_SMALL, pct_str);

    // Compact battery indicator drawn between name and water % when there's
    // room. Format: "B87". Omitted on waiting/stale rows where the water-status
    // text already conveys state.
    if (has_data && !waiting && !stale && data.battery_pct >= 0 && data.battery_pct <= 100) {
        char bat_str[8];
        snprintf(bat_str, sizeof(bat_str), "B%d", data.battery_pct);
        int bat_w = disp_text_width(FONT_SMALL, bat_str);
        int bat_x = pct_x - bat_w - 4;  // 4 px gap before water %
        // Only draw if there's enough room left of the name (4 chars min)
        int name_pixels = (int)strlen(n) * 6;
        if (bat_x > x0 + name_pixels + 4) {
            disp_text(bat_x, text_y, FONT_SMALL, bat_str);
        }
    }

    // Bar (full width minus attention stripe + focus caret reserve)
    int bar_x = x0;
    int bar_w = 128 - x0 - (is_focused ? 4 : 1);
    int pct = waiting ? 0 : (stale ? 0 : data.water_pct);

    if (waiting || stale) {
        // Outline-only bar to differentiate from healthy/low at a glance
        disp_rect(bar_x, bar_y, bar_w, bar_h, true);
    } else {
        // Filled progress bar (proportional fill)
        disp_progress_bar(bar_x, bar_y, bar_w, bar_h, pct);
    }
}

// ── SCREEN_WATER (rebuilt 2026-04-28): adaptive list view of all tanks ───────
// Replaces the per-tank single-screen rotation. With 1 tank the layout matches
// the prior big-screen detail (justified — no value to a 16 px row when 64 px
// is available). With 2-4 tanks the body is split into equal-height rows.
// With 5+ tanks the list paginates 4 at a time, advancing every screen-tick.
//
// Why permanent overview > per-tank rotation: on a 1.3" panel viewed across
// the kitchen, a horizontal bar in a 16 px row reads as fast as a giant
// FONT_LARGE %, and showing all tanks simultaneously means "are any low?"
// is answered in <1 sec — the actual UX goal. Rotation through per-tank
// "big number" screens forced users to wait up to 160 s with 4 tanks.
static int s_water_page = 0;  // pagination cursor for >4 tanks; advanced by display_task
static void draw_water_screen_list(int tx_count) {
    if (tx_count == 0) return;

    // Single-tank → keep the big-screen layout (more readable than a 16 px row)
    if (tx_count == 1) {
        tx_info_t info; tx_data_t data;
        if (!registry_get_info(0, &info)) return;
        bool has_data = registry_get_data(0, &data) && data.data_valid;
        bool stale    = has_data && (data.state == TX_STATE_STALE || data.state == TX_STATE_LOST);
        bool ok       = has_data && !stale;

        // Tank name (top, FONT_SMALL = 8 px tall, occupies y=12..19)
        char n[TX_NAME_MAX]; strncpy(n, info.name, 12); n[12] = 0;
        disp_text(0, 12, FONT_SMALL, n);

        // Capsule body at y=23..56 (33 px tall) — leaves 3 px gap above the
        // name and stays inside its bounding box, no protrusion (no lid).
        disp_tank_capsule(4, 23, 40, 33, ok ? data.water_pct : 0);

        if (ok) {
            char pct_str[4];
            snprintf(pct_str, sizeof(pct_str), "%d", data.water_pct);
            int digits = (int)strlen(pct_str);
            int pct_x = (digits == 3) ? 50 : (digits == 2) ? 56 : 68;
            // LARGE % number aligned to the top of the capsule (y=23..46)
            disp_text(pct_x, 23, FONT_LARGE, pct_str);
            disp_text(pct_x + digits * 18, 23, FONT_SMALL, "%");

            char liters[22];
            snprintf(liters, sizeof(liters), "%dL/%dL",
                     (int)data.water_liters, (int)info.capacity_liters);
            // Liters lifted to y=40 to free the bottom row for battery + age.
            disp_text(50, 40, FONT_SMALL, liters);

            // Battery line at y=49 — "B87 chg" when INA219 + charging,
            // "B87 4.0V" when voltage data is present, "B87%" otherwise.
            // Mirrors the multi-tank list format so the user sees consistent
            // battery info regardless of which screen is up.
            char bat[16] = {0};
            if (data.power_mode == 'i' && data.charging) {
                snprintf(bat, sizeof(bat), "B%d%% chg", data.battery_pct);
            } else if (data.battery_voltage > 0.0f) {
                int v_int = (int)data.battery_voltage;
                int v_dec = (int)(data.battery_voltage * 10.0f) % 10;
                snprintf(bat, sizeof(bat), "B%d %d.%dV", data.battery_pct, v_int, v_dec);
            } else {
                snprintf(bat, sizeof(bat), "B%d%%", data.battery_pct);
            }
            disp_text(50, 49, FONT_SMALL, bat);

            if (data.last_update_us > 0) {
                int64_t age_s = (esp_timer_get_time() - data.last_update_us) / 1000000LL;
                char ago[16];
                if (age_s < 60)       snprintf(ago, sizeof(ago), "%ds", (int)age_s);
                else if (age_s < 3600) snprintf(ago, sizeof(ago), "%dm", (int)(age_s / 60));
                else                   snprintf(ago, sizeof(ago), "%dh", (int)(age_s / 3600));
                // Last-seen on the very bottom row, right-aligned. 4-char max ("999h").
                int aw = disp_text_width(FONT_SMALL, ago);
                disp_text(128 - aw - 1, 57, FONT_SMALL, ago);
            }
        } else {
            disp_text(56, 30, FONT_MEDIUM, "--");
            disp_text(50, 48, FONT_SMALL,
                      (has_data && stale) ? "Stale" : "Waiting...");
        }
        return;
    }

    // 2-4 tanks → adaptive row heights (full body, 53 px below status bar)
    // 5+ tanks → fixed 13 px rows showing 4 at a time, paginated
    const int body_top = 12;   // first usable y after status bar (which ends at y=10)
    const int body_h   = 52;   // 64 - 12

    int visible = tx_count;
    int start = 0;
    int focus_idx = -1;
    if (tx_count > 4) {
        visible = 4;
        start = (s_water_page * 4) % tx_count;
        // If page lands such that fewer than 4 remain, clamp start so we
        // always show 4 rows (overlapping the previous page's tail) — keeps
        // row geometry consistent across pages.
        if (start + visible > tx_count) start = tx_count - visible;
        focus_idx = -1;  // pagination marker is on the row, not a focus caret
    }

    int row_h = body_h / visible;
    if (row_h > 26) row_h = 26;  // 2 tanks → 26 px each is tall enough; rest of body stays empty

    for (int i = 0; i < visible; i++) {
        int idx = start + i;
        int y = body_top + i * row_h;
        draw_tank_row(idx, y, row_h, focus_idx == idx);
    }

    // Page indicator dot row at very bottom for 5+ tanks
    if (tx_count > 4) {
        int pages = (tx_count + 3) / 4;
        int cur_page = (start / 4);
        int dot_y = 63;
        int dot_w = 4, dot_gap = 3;
        int total_w = pages * dot_w + (pages - 1) * dot_gap;
        int x = (128 - total_w) / 2;
        for (int p = 0; p < pages; p++) {
            if (p == cur_page) disp_fill_rect(x, dot_y, dot_w, 1, true);
            else               disp_pixel(x + dot_w/2, dot_y, true);
            x += dot_w + dot_gap;
        }
    }
}

// ── Health summary: battery + signal at a glance for all tanks ──────────────
// Replaces the per-tank SCREEN_BATTERY + SCREEN_SIGNAL + SCREEN_DIAGNOSTICS
// rotation. One screen, all tanks. Two columns when >3 tanks.
//   Each cell: name (truncated) | battery icon + % | RSSI dBm
static void draw_health_summary(int tx_count) {
    disp_text(0, 12, FONT_SMALL, "HEALTH");
    disp_hline(0, 21, 128, true);

    bool two_col = (tx_count > 3);
    int row_h = 10, y0 = 23;

    for (int i = 0; i < tx_count && i < 8; i++) {
        tx_info_t info; tx_data_t data;
        if (!registry_get_info(i, &info)) continue;
        bool has = registry_get_data(i, &data) && data.data_valid;
        int x = two_col ? (i % 2) * 64 : 0;
        int y = y0 + (two_col ? (i / 2) : i) * row_h;
        if (y > 56) break;

        char line[22];
        if (has) {
            // Single-letter status ('+' charging, ' ' otherwise) keeps line short.
            char chg = data.charging ? '+' : ' ';
            // Compact: T1 87% +-71  (two_col) or  Rooftop 87%+ -71 dBm (one col)
            if (two_col) {
                snprintf(line, sizeof(line), "T%d %d%%%c%d", i + 1, data.battery_pct, chg, data.rssi);
            } else {
                char n[9] = {0}; strncpy(n, info.name, 8);
                snprintf(line, sizeof(line), "%-8s %d%%%c %ddBm", n, data.battery_pct, chg, data.rssi);
            }
        } else {
            if (two_col) snprintf(line, sizeof(line), "T%d --", i + 1);
            else         snprintf(line, sizeof(line), "T%d  no data", i + 1);
        }
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
// Setup-state screens — shown when not yet usable for tank monitoring.
//   AP-mode  : guide user to join SoftAP and open the captive page
//   Unclaimed: device on home Wi-Fi but no cloud creds — show device_id big
// Both keep the device_id readable so the user can recover it even if the
// QR sticker on the box is missing.
// ─────────────────────────────────────────────────────────────────────────────
static void chunked_device_id(char *out, size_t cap) {
    const char *id = mqtt_manager_device_id();
    if (!id || strlen(id) != 12) { snprintf(out, cap, "%s", id ? id : "????????????"); return; }
    char up[13]; for (int i = 0; i < 12; i++) up[i] = (id[i] >= 'a' && id[i] <= 'z') ? id[i] - 32 : id[i];
    up[12] = 0;
    snprintf(out, cap, "%c%c%c%c-%c%c%c%c-%c%c%c%c",
             up[0],up[1],up[2],up[3],up[4],up[5],up[6],up[7],up[8],up[9],up[10],up[11]);
}

static void draw_ap_setup_screen(void) {
    disp_text(4, 2, FONT_MEDIUM, "SETUP WIFI");
    disp_text(0, 24, FONT_SMALL, "Join Wi-Fi:");
    disp_text(0, 34, FONT_SMALL, wifi_manager_ssid());      // "TankSync-XXXX" in AP mode
    disp_text(0, 46, FONT_SMALL, "Open 192.168.4.1");
    char id[16]; chunked_device_id(id, sizeof(id));
    char line[24]; snprintf(line, sizeof(line), "ID:%s", id);
    disp_text(0, 56, FONT_SMALL, line);
}

static void draw_unclaimed_screen(void) {
    disp_text(4, 2, FONT_MEDIUM, "NOT LINKED");
    disp_text(0, 22, FONT_SMALL, "DEVICE ID");
    char id[16]; chunked_device_id(id, sizeof(id));
    disp_rect(0, 31, 90, 12, true);
    disp_text(2, 33, FONT_SMALL, id);                       // bordered for emphasis
    disp_text(0, 47, FONT_SMALL, "Scan QR on box");
    disp_text(0, 56, FONT_SMALL, wifi_manager_ip());
}

static void draw_connecting_screen(void) {
    disp_text(20, 16, FONT_MEDIUM, "WIFI...");
    int dots = (xTaskGetTickCount() / pdMS_TO_TICKS(500)) % 4;
    char ds[5] = {0}; for (int i = 0; i < dots; i++) ds[i] = '.';
    disp_text(55, 40, FONT_SMALL, ds);
}

// Linked + on Wi-Fi but no TX paired yet: show device_id + IP + pairing hint
// instead of falling through to the screen-rotation that just renders
// "No transmitters" on three different per-tank screens.
static void draw_no_tx_screen(void) {
    disp_text(4, 2, FONT_MEDIUM, "ADD A TANK");
    char id[16]; chunked_device_id(id, sizeof(id));
    disp_rect(0, 21, 90, 12, true);
    disp_text(2, 23, FONT_SMALL, id);
    disp_text(0, 37, FONT_SMALL, "Pair: hold TX BTN");
    disp_text(0, 47, FONT_SMALL, "for 2s after wake");
    disp_text(0, 56, FONT_SMALL, wifi_manager_ip());
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

    // ── Animated boot splash (~1.5s) ────────────────────────────────────────
    // Tank graphic fills from 0% to 100% as the title types in character by
    // character. Cosmetic — adds ~600 bytes compiled, well within the 21%
    // OTA partition headroom (316 KB free of 1.5 MB).
    {
        const char *title = "TankSync";
        const int title_len = 8;
        const int title_w = disp_text_width(FONT_MEDIUM, title);
        const int title_x = (DISPLAY_WIDTH - title_w) / 2;
        const char *ver = "v" FIRMWARE_VERSION;
        const int ver_w = disp_text_width(FONT_SMALL, ver);
        const int ver_x = (DISPLAY_WIDTH - ver_w) / 2;

        // Tank graphic geometry: centered horizontally, mid-screen vertically
        const int tank_w = 36, tank_h = 24;
        const int tank_x = (DISPLAY_WIDTH - tank_w) / 2;
        const int tank_y = 22;

        // Phase 1: type title in (one char every ~25ms = ~200ms total)
        for (int n = 1; n <= title_len; n++) {
            disp_clear();
            char buf[16];
            strncpy(buf, title, n);
            buf[n] = '\0';
            int partial_w = disp_text_width(FONT_MEDIUM, buf);
            // Keep title centered relative to its FINAL position so it doesn't
            // jump around — pad on left as we type (looks like a reveal).
            disp_text(title_x + (title_w - partial_w) / 2, 4, FONT_MEDIUM, buf);
            disp_tank_capsule(tank_x, tank_y, tank_w, tank_h, 0);
            disp_flush();
            vTaskDelay(pdMS_TO_TICKS(25));
        }

        // Phase 2: fill the tank from 0% → 100% (~800ms)
        for (int pct = 0; pct <= 100; pct += 4) {
            disp_clear();
            disp_text(title_x, 4, FONT_MEDIUM, title);
            disp_tank_capsule(tank_x, tank_y, tank_w, tank_h, pct);
            disp_flush();
            vTaskDelay(pdMS_TO_TICKS(25));
        }

        // Phase 3: reveal version + hold (~500ms)
        disp_clear();
        disp_text(title_x, 4, FONT_MEDIUM, title);
        disp_tank_capsule(tank_x, tank_y, tank_w, tank_h, 100);
        disp_text(ver_x, 54, FONT_SMALL, ver);
        disp_flush();
        vTaskDelay(pdMS_TO_TICKS(500));
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
            // Wrapped back to first screen — advance the water-list pagination
            // cursor so 5+ tank installations cycle through pages on each loop.
            // For ≤4 tanks the page is always 0 (no pagination needed) so this
            // is a no-op.
            if (scr_idx <= prev && tx_count > 4) {
                int pages = (tx_count + 3) / 4;
                s_water_page = (s_water_page + 1) % pages;
            }
            tank_idx = 0;

            // Reload mask so web UI changes take effect on next cycle
            scr_mask = load_display_mask();
            build_screen_list(scr_mask, scr_list, &scr_count);
            if (scr_count == 0) { scr_mask = 0x1F; build_screen_list(scr_mask, scr_list, &scr_count); }
            if (scr_idx >= scr_count) scr_idx = 0;
        }

        // tank_idx is no longer used by the list-view layout (all tanks shown at
        // once), but is kept for compatibility with the surrounding setup-state
        // screens — leaving it as 0.
        (void)tank_idx;

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
        } else if (ws == WIFI_ST_AP_MODE) {
            draw_ap_setup_screen();
        } else if (ws == WIFI_ST_CONNECTED && !mqtt_manager_is_linked()) {
            draw_unclaimed_screen();
        } else if (ws != WIFI_ST_CONNECTED) {
            draw_connecting_screen();
        } else if (tx_count == 0) {
            draw_no_tx_screen();
        } else {
            draw_status_bar(ws, tx_count);
            draw_nav_dots(scr_idx, scr_count);

            // Screen enum positions kept stable for NVS-stored display masks
            // (existing devices have bits 0-4 set). Bits SCREEN_SIGNAL and
            // SCREEN_DIAGNOSTICS are now treated as duplicates of SCREEN_BATTERY
            // (which now means "Health summary"), so they fall through harmlessly.
            switch (cur_scr) {
                case SCREEN_WATER:
                    draw_water_screen_list(tx_count);
                    break;
                case SCREEN_BATTERY:
                case SCREEN_SIGNAL:
                case SCREEN_DIAGNOSTICS:
                    draw_health_summary(tx_count);
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
            ESP_LOGW(TAG, "Display I2C failed (%s) — retrying in 5s",
                     esp_err_to_name(flush_err));
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_UPDATE_MS));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// LED task — reflects system state on 2, 8, or 24 LED strip/ring
// ─────────────────────────────────────────────────────────────────────────────

// Auto-palette: 6 distinct base colors for tanks (indexed 0-5)
// Each ~60° apart on hue wheel — unmistakable even at low brightness
static const led_color_t TANK_PALETTE[] = {
    {   0, 100, 255 },  // 0: Blue
    {   0, 255,   0 },  // 1: Green
    { 255, 100,   0 },  // 2: Orange
    { 150,   0, 255 },  // 3: Purple
    { 255, 200,   0 },  // 4: Yellow
    { 200, 200, 200 },  // 5: White
};
#define PALETTE_COUNT  (sizeof(TANK_PALETTE) / sizeof(TANK_PALETTE[0]))

static led_color_t dim_color(led_color_t c, uint8_t pct) {
    return (led_color_t){ c.r * pct / 100, c.g * pct / 100, c.b * pct / 100 };
}

// Get color for a tank based on its water level and assigned/auto palette index
static led_color_t tank_level_color(int water_pct, tx_state_t state, int8_t color_idx, int slot,
                                     TickType_t tick) {
    // Offline/stale: dim white blink
    if (state == TX_STATE_LOST || state == TX_STATE_WAITING) {
        return ((tick / pdMS_TO_TICKS(800)) % 2) ? dim_color(LED_WHITE, 15) : LED_OFF;
    }
    if (state == TX_STATE_STALE) {
        return ((tick / pdMS_TO_TICKS(1200)) % 2) ? dim_color(LED_WHITE, 25) : LED_OFF;
    }

    // Pick base color: user override or auto from palette
    int ci = (color_idx >= 0 && color_idx < (int)PALETTE_COUNT) ? color_idx : (slot % PALETTE_COUNT);
    led_color_t base = TANK_PALETTE[ci];

    // Critical: red pulse (overrides everything)
    if (water_pct <= 5) {
        uint8_t pulse = 40 + (uint8_t)(40.0f * ((tick / pdMS_TO_TICKS(50)) % 20 < 10
            ? (float)((tick / pdMS_TO_TICKS(50)) % 10) / 10.0f
            : 1.0f - (float)((tick / pdMS_TO_TICKS(50)) % 10) / 10.0f));
        return dim_color(LED_RED, pulse);
    }
    // Low: amber warning
    if (water_pct <= 20) return LED_AMBER;
    // Mid: half brightness base
    if (water_pct <= 50) return dim_color(base, 50);
    // Good: full brightness base
    return base;
}

static void led_task(void *arg) {
    // Read LED config from NVS
    uint8_t led_count = LED_COUNT_DEFAULT;
    uint8_t led_brightness = LED_BRIGHTNESS_DEFAULT;
    {
        nvs_handle_t h;
        if (nvs_open(NVS_NS_SYSTEM, NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, "led_count", &led_count);
            nvs_get_u8(h, "led_bright", &led_brightness);
            nvs_close(h);
        }
    }
    // Validate
    if (led_count != 2 && led_count != 8 && led_count != 24) led_count = LED_COUNT_DEFAULT;
    if (led_brightness == 0) led_brightness = LED_BRIGHTNESS_DEFAULT;

    led_init(PIN_LED_DATA, led_count, led_brightness);

    TickType_t  blink_tick  = xTaskGetTickCount();
    bool        blink_on    = true;
    uint32_t    data_flash  = 0;

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        wifi_status_t ws = wifi_manager_status();

        // ── WiFi LED (always index 0) ──────────────────────────────────────
        led_color_t wifi_col;
        if (ws == WIFI_ST_CONNECTED && mqtt_manager_status() == MQTT_ST_CONNECTED) {
            wifi_col = LED_GREEN;                       // full connection
        } else if (ws == WIFI_ST_CONNECTED) {
            wifi_col = LED_YELLOW;                      // WiFi OK, MQTT not yet
        } else if (ws == WIFI_ST_AP_MODE) {
            if ((now - blink_tick) >= pdMS_TO_TICKS(1000)) { blink_tick = now; blink_on = !blink_on; }
            wifi_col = blink_on ? LED_BLUE : LED_OFF;
        } else if (ws == WIFI_ST_CONNECTING) {
            if ((now - blink_tick) >= pdMS_TO_TICKS(250)) { blink_tick = now; blink_on = !blink_on; }
            wifi_col = blink_on ? LED_YELLOW : LED_OFF;
        } else {
            wifi_col = LED_RED;
        }

        // ── LoRa data flash (white burst on any packet) ───────────────────
        EventBits_t bits = xEventGroupWaitBits(s_events, EVT_NEW_LORA_DATA, pdTRUE, pdFALSE, 0);
        if (bits & EVT_NEW_LORA_DATA) data_flash = 300;
        bool lora_flashing = data_flash > 0;
        if (data_flash > 0) data_flash = (data_flash > 100) ? data_flash - 100 : 0;

        // ── OTA override ──────────────────────────────────────────────────
        bool ota_active = xEventGroupGetBits(s_events) & EVT_OTA_IN_PROGRESS;

        // ────────────────────────────────────────────────────────────────────
        // MODE: 2 LEDs (legacy — combined status + worst tank)
        // ────────────────────────────────────────────────────────────────────
        if (led_count == 2) {
            led_color_t water_col;
            int online = registry_online_count();
            if (online == 0) {
                water_col = LED_OFF;
            } else {
                int min_pct = 100;
                tx_state_t worst = registry_worst_state();
                for (int i = 0; i < registry_count(); i++) {
                    tx_info_t info; tx_data_t data;
                    if (!registry_get_info(i, &info) || !info.enabled) continue;
                    if (!registry_get_data(i, &data) || data.state != TX_STATE_CONNECTED) continue;
                    if (data.water_pct < min_pct) min_pct = data.water_pct;
                }
                if (worst == TX_STATE_LOST || worst == TX_STATE_WAITING) water_col = LED_WHITE;
                else if (min_pct > 50) water_col = LED_GREEN;
                else if (min_pct > 20) water_col = LED_YELLOW;
                else water_col = LED_RED;
            }
            if (lora_flashing) wifi_col = LED_WHITE;
            if (ota_active) {
                bool b = ((now / pdMS_TO_TICKS(200)) % 2);
                wifi_col = b ? LED_ORANGE : LED_OFF;
                water_col = b ? LED_OFF : LED_ORANGE;
            }
            led_set(0, wifi_col);
            led_set(1, water_col);
        }

        // ────────────────────────────────────────────────────────────────────
        // MODE: 8 LEDs (strip — WiFi + LoRa + 6 tanks)
        // ────────────────────────────────────────────────────────────────────
        else if (led_count == 8) {
            // LED 0 = WiFi
            led_set(0, wifi_col);

            // LED 1 = LoRa
            led_color_t lora_col;
            if (lora_flashing) {
                lora_col = LED_WHITE;
            } else {
                int online = registry_online_count();
                bool pairing = lora_is_pairing_mode();
                if (pairing) {
                    lora_col = ((now / pdMS_TO_TICKS(400)) % 2) ? LED_BLUE : LED_CYAN;
                } else if (online > 0) {
                    lora_col = LED_GREEN;
                } else if (registry_count() > 0) {
                    lora_col = LED_AMBER;  // paired but no data
                } else {
                    lora_col = dim_color(LED_RED, 30);  // no TX paired
                }
            }
            led_set(1, lora_col);

            // LED 2-7 = Tank 1-6
            int n_tanks = registry_count();
            for (int slot = 0; slot < 6; slot++) {
                if (slot >= n_tanks) {
                    led_set(LED_IDX_TANK_START + slot, LED_OFF);
                    continue;
                }
                tx_info_t info; tx_data_t data;
                if (!registry_get_info(slot, &info) || !info.enabled) {
                    led_set(LED_IDX_TANK_START + slot, LED_OFF);
                    continue;
                }
                registry_get_data(slot, &data);
                led_set(LED_IDX_TANK_START + slot,
                    tank_level_color(data.water_pct, data.state, info.led_color_idx, slot, now));
            }

            // OTA override: chase pattern across all LEDs
            if (ota_active) {
                int chase = (int)((now / pdMS_TO_TICKS(150)) % 8);
                for (int i = 0; i < 8; i++) {
                    led_set(i, (i == chase) ? LED_ORANGE : dim_color(LED_ORANGE, 10));
                }
            }
        }

        // ────────────────────────────────────────────────────────────────────
        // MODE: 24 LEDs (ring — WiFi + LoRa + gauge segments per tank)
        // ────────────────────────────────────────────────────────────────────
        else if (led_count == 24) {
            // LED 0 = WiFi (12 o'clock left)
            led_set(0, wifi_col);

            // LED 23 = LoRa (12 o'clock right)
            led_color_t lora_col;
            if (lora_flashing) {
                lora_col = LED_WHITE;
            } else {
                int online = registry_online_count();
                bool pairing = lora_is_pairing_mode();
                if (pairing) lora_col = ((now / pdMS_TO_TICKS(400)) % 2) ? LED_BLUE : LED_CYAN;
                else if (online > 0) lora_col = LED_GREEN;
                else if (registry_count() > 0) lora_col = LED_AMBER;
                else lora_col = dim_color(LED_RED, 30);
            }
            led_set(23, lora_col);

            // LEDs 1-22 = tank gauges (22 LEDs split among tanks)
            int n_tanks = registry_count();
            if (n_tanks == 0) {
                for (int i = 1; i <= 22; i++) led_set(i, LED_OFF);
            } else {
                int leds_per_tank = 22 / n_tanks;
                int remainder = 22 - (leds_per_tank * n_tanks);
                int led_idx = 1;

                for (int slot = 0; slot < n_tanks && slot < 6; slot++) {
                    int seg_len = leds_per_tank + (slot < remainder ? 1 : 0);
                    tx_info_t info; tx_data_t data;
                    bool valid = registry_get_info(slot, &info) && info.enabled;
                    if (valid) registry_get_data(slot, &data);

                    if (!valid) {
                        for (int j = 0; j < seg_len; j++) led_set(led_idx++, LED_OFF);
                        continue;
                    }

                    // Gauge fill: number of LEDs lit proportional to water level
                    int fill = (data.state == TX_STATE_CONNECTED)
                        ? (data.water_pct * seg_len + 50) / 100  // round
                        : 0;
                    led_color_t base_col = tank_level_color(
                        data.water_pct, data.state, info.led_color_idx, slot, now);
                    led_color_t dim_bg = dim_color(base_col, 8);  // very dim background

                    for (int j = 0; j < seg_len; j++) {
                        // Fill from bottom of segment (last LED in segment = bottom)
                        bool lit = j < fill;
                        led_set(led_idx++, lit ? base_col : dim_bg);
                    }
                }
                // Fill any remaining LEDs (if fewer than 6 tanks but some remainder)
                while (led_idx <= 22) led_set(led_idx++, LED_OFF);
            }

            // OTA override: rotating chase around the ring
            if (ota_active) {
                int chase = (int)((now / pdMS_TO_TICKS(80)) % 24);
                for (int i = 0; i < 24; i++) {
                    int dist = (i - chase + 24) % 24;
                    uint8_t br = (dist < 4) ? (100 - dist * 25) : 5;
                    led_set(i, dim_color(LED_ORANGE, br));
                }
            }
        }

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
// History timer — drives 30-min slot capture + hourly NVS persist.
// Runs in the esp_timer task context (a single shared worker thread for all
// esp_timer callbacks), so we keep the body short and blocking-free; the
// registry's own mutex serialises against LoRa-RX writes.
// ─────────────────────────────────────────────────────────────────────────────
static void history_timer_cb(void *arg) {
    (void)arg;
    // Buzzer repeat cadence runs even before SNTP — it uses monotonic time
    // (esp_timer_get_time), not wall-clock.
    buzz_check_repeats();

    if (!geo_time_is_synced()) return;       // wait for wall-clock before sampling
    int64_t now = (int64_t)time(NULL);
    if (now <= 0) return;

    registry_history_tick(now);

    // Persist hourly. Tracking by epoch-hour means we always write at most
    // once per wall-clock hour even if the tick is jittered. The first persist
    // after boot waits one full hour — fine, since hist_load just ran on init.
    static int64_t s_last_persist_hour = -1;
    int64_t cur_hour = now / 3600;
    if (s_last_persist_hour < 0) s_last_persist_hour = cur_hour;
    if (cur_hour != s_last_persist_hour) {
        registry_history_persist();          // skip-if-clean is enforced internally
        s_last_persist_hour = cur_hour;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// app_main
// ─────────────────────────────────────────────────────────────────────────────
void app_main(void) {
    // Install the log ring buffer FIRST so even the boot banner is captured
    // and served via /api/logs to the dashboard / curl users. 4 KB in RAM,
    // forwards every line through to the original sink (UART console + USB
    // serial monitor) so existing workflows are untouched.
    log_buffer_init();

    ESP_LOGI(TAG, "TankSync Receiver v%s booting (build %08X)", FIRMWARE_VERSION, (unsigned)TS_BUILD_FP);

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

    // ── 3b. Buzzer driver — needs NVS only (no network deps). Boot tone fires
    // at end of app_main, after all subsystems initialize, so the user hears
    // "I'm fully alive" rather than "init started".
    ESP_ERROR_CHECK(buzzer_init(PIN_BUZZER));

    // ── 4. WiFi ──
    ESP_ERROR_CHECK(wifi_manager_init(s_events));
    wifi_manager_connect();  // STA if creds saved, AP fallback

    // ── 4b. SNTP wall-clock + IP-geolocation country code ──
    // Spawns a background task that waits for WiFi and then sets up SNTP
    // and one-shot ip-api.com country detect. Used by web UI for LoRa
    // compliance hints + future hourly-history-bucket alignment.
    ESP_ERROR_CHECK(geo_time_init());

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

    // ── 10. Half-hourly history tick (Phase 2) ──
    // Fires every 60s; the tick fn no-ops within the same 30-min bucket. Once
    // SNTP syncs, slot capture begins. Persists to /spiffs/tx_hist.bin every
    // hour (skip-if-clean) so reboots don't wipe the rolling 24-h window.
    {
        const esp_timer_create_args_t hist_args = {
            .callback = &history_timer_cb,
            .name     = "hist_tick",
        };
        esp_timer_handle_t hist_timer = NULL;
        if (esp_timer_create(&hist_args, &hist_timer) == ESP_OK) {
            esp_timer_start_periodic(hist_timer, 60ULL * 1000000ULL);
            ESP_LOGI(TAG, "History tick started (60s period)");
        } else {
            ESP_LOGW(TAG, "Failed to create history timer — predictions will be live-only");
        }
    }

    ESP_LOGI(TAG, "All tasks started. Free heap: %lu bytes",
             (unsigned long)esp_get_free_heap_size());

    // Boot tone — bypasses master_enable + quiet hours by design.
    // Confirms "I'm on, I'm working" — useful for setup, post-outage, post-OTA.
    buzzer_play(BUZZ_BOOT_COMPLETE);

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

    // NOTE: semaphores are drained in on_lora_rx BEFORE this task is spawned.

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

    // Switch to fast LoRa params for chunk streaming
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

        // Wait for OTA_DONE at SF7 — TX needs 2-3s for SHA256 verify.
        // We MUST stay on SF7 until we hear it, otherwise TX sends OTA_DONE
        // at SF7 and we can't hear it on SF9.
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
