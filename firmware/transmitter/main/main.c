// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

/**
 * TankSync Transmitter v2 — app_main
 *
 * Operation cycle (runs on every wake from deep sleep):
 *   1. Init peripherals (NVS, UART, ADC, GPIO)
 *   2. Read ultrasonic sensor (IQR filtered median)
 *   3. Read battery (ADC averaged)
 *   4. Initialize LoRa module, send packet with retries
 *   5. Listen for downlink (config/OTA) if ACK received
 *   6. Enter deep sleep
 *
 * On timer wake: skips boot window entirely for battery savings.
 * On power-on/reset: 10s boot window for pairing/diagnostics.
 */

#include "config.h"
#include "sensor_sr04.h"
#include "battery_monitor.h"
#include "lora_tx.h"
#include "wifi_ota.h"
#include "led_ws2812.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "main";

// ── Helpers ──────────────────────────────────────────────────────────────────
#define SEND_STR(s)  lora_tx_send_raw((s), strlen(s))
#define CLAMP(x, lo, hi)  ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))

// Low-battery threshold — skip TX and extend sleep to protect LiPo
#define BAT_CUTOFF_MV  3100
#define BAT_CUTOFF_SLEEP_S  3600  // 1 hour

// OTA total timeout — prevent staying awake indefinitely on stalled OTA
#define OTA_TOTAL_TIMEOUT_MS  (5 * 60 * 1000)  // 5 minutes

// ── RTC memory (survives deep sleep) ─────────────────────────────────────────
RTC_DATA_ATTR static uint32_t s_msg_id    = 0;
RTC_DATA_ATTR static uint32_t s_boot_count = 0;
RTC_DATA_ATTR static uint32_t s_ack_failures = 0;

// ── Simple LED flash ──────────────────────────────────────────────────────────
static void led_flash(int pin, int on_ms, int count) {
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    for (int i = 0; i < count; i++) {
        gpio_set_level(pin, 1);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        gpio_set_level(pin, 0);
        if (i < count - 1) vTaskDelay(pdMS_TO_TICKS(on_ms));
    }
}

// ── Diagnostic mode ───────────────────────────────────────────────────────────
static void diagnostic_mode(void) {
    ESP_LOGI(TAG, "=== DIAGNOSTIC MODE ===");
    ESP_LOGI(TAG, "boot=%" PRIu32 " msg_id=%" PRIu32 " ack_fail=%" PRIu32,
             s_boot_count, s_msg_id, s_ack_failures);

    sensor_init(PIN_TRIG, PIN_ECHO);
    power_init(BAT_ADC_CHANNEL, PIN_I2C_SDA, PIN_I2C_SCL);
    ESP_LOGI(TAG, "Power monitor mode: %s", power_mode_str(power_get_mode()));

    for (;;) {
        lora_tx_config_t cfg; lora_tx_get_config(&cfg);
        int dist_cm = -1;
        esp_err_t serr = sensor_read_cm(&dist_cm);
        power_reading_t pr = {0};
        power_read(&pr);

        ESP_LOGI(TAG, "dist=%dcm(%s) bat=%d%% %.2fV (%s%s%ldmA) lora_addr=%d rx_addr=%d",
                 dist_cm, serr == ESP_OK ? "ok" : "err",
                 pr.pct, (float)pr.vbat_mv / 1000.0f,
                 power_mode_str(pr.mode), pr.charging ? " CHG " : " ",
                 (long)pr.current_ma,
                 cfg.my_address, cfg.receiver_address);

        if (serr == ESP_OK && dist_cm > 0) {
            s_msg_id++;
            bool acked = lora_tx_send(dist_cm, pr.pct,
                                      (float)pr.vbat_mv / 1000.0f,
                                      power_mode_char(pr.mode),
                                      pr.current_ma, pr.power_mw,
                                      s_msg_id);
            led_flash(PIN_LED, 100, acked ? 2 : 5);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ── Remote Config & OTA handler ──────────────────────────────────────────────
static void handle_downlink(void) {
    // Use static buffer to reduce stack pressure (audit issue #9)
    static char line[512];
    int pos = 0;
    ESP_LOGI(TAG, "Checking for downlink (Config/OTA)...");

    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(8000);
    while (xTaskGetTickCount() < end) {
        uint8_t ch;
        if (lora_tx_read_raw(&ch, pdMS_TO_TICKS(50)) <= 0) continue;
        if (ch == '\r') continue;
        if (ch == '\n') {
            line[pos] = '\0'; pos = 0;
            vTaskDelay(pdMS_TO_TICKS(1));  // yield to feed WDT (not just taskYIELD)
            if (strncmp(line, "+RCV=", 5) == 0) {
                // ── CONFIG: SET:SLEEP=N:SAMP=N ──
                char *payload = strstr(line, "SET:");
                if (payload) {
                    ESP_LOGI(TAG, "CONFIG: %s", payload);
                    char *s_sleep = strstr(payload, "SLEEP=");
                    char *s_samp  = strstr(payload, "SAMP=");
                    nvs_handle_t h;
                    if (nvs_open(NVS_NS_SYSTEM, NVS_READWRITE, &h) == ESP_OK) {
                        if (s_sleep) {
                            uint32_t val = CLAMP(atoi(s_sleep + 6), 60, 86400);
                            nvs_set_u32(h, "sleep_s", val);
                        }
                        if (s_samp) {
                            uint8_t val = CLAMP(atoi(s_samp + 5), 3, 20);
                            nvs_set_u8(h, "samples", val);
                        }
                        nvs_commit(h); nvs_close(h);
                    }
                    SEND_STR("SET_ACK");
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }

                // ── OTA: OTA_START:<size> ──
                payload = strstr(line, "OTA_START:");
                if (payload) {
                    uint32_t total_size = 0;
                    sscanf(payload, "OTA_START:%" PRIu32, &total_size);
                    ESP_LOGI(TAG, "OTA START: %" PRIu32 " bytes", total_size);

                    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
                    if (!update_part) {
                        ESP_LOGE(TAG, "No OTA partition found");
                        SEND_STR("OTA_ERR:NO_PART");
                        return;
                    }

                    esp_ota_handle_t ota_h;
                    // OTA_WITH_SEQUENTIAL_WRITES erases sectors incrementally
                    // during esp_ota_write, NOT upfront. This avoids the 5-15s
                    // bulk erase that triggers Task WDT on ESP32-C3.
                    // (OTA_SIZE_UNKNOWN erases the entire partition — wrong!)
                    esp_err_t ota_err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_h);
                    if (ota_err != ESP_OK) {
                        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ota_err));
                        SEND_STR("OTA_ERR:BEGIN");
                        return;
                    }

                    // Switch to fast LoRa params for chunk streaming.
                    // SF7/BW9(500kHz) is ~23x faster than SF9/BW7(125kHz).
                    // OTA_START was received at normal params; chunks use fast.
                    ESP_LOGI(TAG, "Switching to SF7/500kHz for OTA chunks");
                    lora_tx_send_at("AT+PARAMETER=7,9,1,12", 1500);

                    // Send OTA_READY 3 times for reliability (half-duplex LoRa
                    // means the RX module may miss the first one).
                    ESP_LOGI(TAG, "Sending OTA_READY 3x (at SF7/500kHz)");
                    for (int r = 0; r < 3; r++) {
                        SEND_STR("OTA_READY");
                    }

                    // ── OTA chunk loop ──
                    uint32_t current_offset = 0;
                    TickType_t ota_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(OTA_TOTAL_TIMEOUT_MS);

                    while (current_offset < total_size) {
                        // Check total OTA timeout (audit #6)
                        if (xTaskGetTickCount() > ota_deadline) {
                            ESP_LOGE(TAG, "OTA total timeout (5min) at offset %" PRIu32, current_offset);
                            esp_ota_abort(ota_h);
                            lora_tx_send_at("AT+PARAMETER=9,7,1,12", 1500);
                            SEND_STR("OTA_ERR:TIMEOUT");
                            return;
                        }

                        TickType_t chunk_timeout = xTaskGetTickCount() + pdMS_TO_TICKS(45000);
                        bool got_chunk = false;
                        while (xTaskGetTickCount() < chunk_timeout) {
                            if (lora_tx_read_raw(&ch, pdMS_TO_TICKS(50)) <= 0) continue;
                            if (ch == '\r') continue;
                            if (ch == '\n') {
                                line[pos] = '\0'; pos = 0;
                                vTaskDelay(pdMS_TO_TICKS(1));  // WDT feed

                                if (strncmp(line, "+RCV=", 5) == 0 && strstr(line, "OTA_DATA:")) {
                                    char *p = strstr(line, "OTA_DATA:");
                                    uint32_t chunk_off = 0;
                                    // Limit hex scan to 240 chars (120 bytes max) (audit #2)
                                    char hex[248];
                                    sscanf(p, "OTA_DATA:%" PRIu32 ":%247s", &chunk_off, hex);
                                    // +RCV line has trailing ,rssi,snr — %s doesn't stop at commas.
                                    // Truncate hex at first comma to avoid decoding RSSI as hex data.
                                    char *comma = strchr(hex, ',');
                                    if (comma) *comma = '\0';

                                    if (chunk_off == current_offset) {
                                        uint8_t data[128];
                                        int blen = 0;
                                        int hlen = strlen(hex);
                                        // Cap decoded length (audit #2)
                                        for (int i = 0; i < hlen && blen < (int)sizeof(data); i += 2) {
                                            unsigned int b;
                                            sscanf(hex + i, "%02x", &b);
                                            data[blen++] = (uint8_t)b;
                                        }

                                        // Check write result (audit #7)
                                        esp_err_t werr = esp_ota_write(ota_h, data, blen);
                                        if (werr != ESP_OK) {
                                            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(werr));
                                            esp_ota_abort(ota_h);
                                            SEND_STR("OTA_ERR:WRITE");
                                            return;
                                        }

                                        current_offset += blen;
                                        ESP_LOGI(TAG, "OTA chunk OK: off=%" PRIu32 " blen=%d total=%" PRIu32,
                                                 chunk_off, blen, current_offset);
                                        char ack[32];
                                        snprintf(ack, sizeof(ack), "OTA_ACK:%" PRIu32, current_offset);
                                        SEND_STR(ack);
                                        got_chunk = true;
                                        led_flash(PIN_LED, 20, 1);
                                        break;
                                    }
                                }
                            } else if (pos < (int)sizeof(line) - 1) {
                                line[pos++] = (char)ch;
                            }
                        }
                        if (!got_chunk) {
                            ESP_LOGE(TAG, "OTA chunk timeout at %" PRIu32, current_offset);
                            esp_ota_abort(ota_h);
                            // Restore normal LoRa params before sleeping
                            lora_tx_send_at("AT+PARAMETER=9,7,1,12", 1500);
                            SEND_STR("OTA_ERR:TIMEOUT");
                            return;
                        }
                    }

                    // ── OTA complete ──
                    esp_err_t ota_end_err = esp_ota_end(ota_h);
                    if (ota_end_err == ESP_OK &&
                        esp_ota_set_boot_partition(update_part) == ESP_OK) {
                        ESP_LOGI(TAG, "OTA SUCCESS! Rebooting...");
                        SEND_STR("OTA_DONE");
                        led_flash(PIN_LED, 1000, 2);
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        esp_restart();
                    } else {
                        ESP_LOGE(TAG, "OTA end/boot failed: %s", esp_err_to_name(ota_end_err));
                        SEND_STR("OTA_ERR:END");
                    }
                }
            }
        } else if (pos < (int)sizeof(line) - 1) {
            line[pos++] = (char)ch;
        }
    }
}

// ── app_main ─────────────────────────────────────────────────────────────────
void app_main(void) {
    s_boot_count++;
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    ESP_LOGI(TAG, "TankSync TX v%s boot #%" PRIu32 " (wakeup: %d)",
             FIRMWARE_VERSION, s_boot_count, (int)cause);

    // Mark OTA as valid on successful boot (audit #8 — rollback protection)
    esp_ota_mark_app_valid_cancel_rollback();

    // ── 1. GPIO init ──
    gpio_set_direction(PIN_LED, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LED, 0);
    gpio_set_direction(PIN_BUTTON, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_BUTTON, GPIO_PULLUP_ONLY);

    // ── 1a. WS2812B init (2 LEDs on GPIO7) ──
    led_init(PIN_WS2812, PIN_WS2812_COUNT, 40);

    // ── 1b. Button-held check — WiFi OTA mode ──
    // Works on ANY wake (timer, GPIO, power-on). User just presses & holds.
    // If button is pressed at boot, wait 3s. If still held → WiFi OTA.
    if (gpio_get_level(PIN_BUTTON) == 0) {
        ESP_LOGI(TAG, "Button held at boot — hold 3s for WiFi OTA...");
        for (int i = 0; i < 30; i++) {  // 30 × 100ms = 3s
            gpio_set_level(PIN_LED, (i % 2));  // fast blink
            vTaskDelay(pdMS_TO_TICKS(100));
            if (gpio_get_level(PIN_BUTTON) != 0) {
                ESP_LOGI(TAG, "Button released — normal boot");
                gpio_set_level(PIN_LED, 0);
                break;
            }
            if (i == 29) {
                ESP_LOGI(TAG, "Button held 3s — entering WiFi OTA mode");
                gpio_set_level(PIN_LED, 0);
                led_flash(PIN_LED, 50, 10);
                // NVS must be init before wifi_ota (it reads settings + address)
                esp_err_t nv = nvs_flash_init();
                if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
                    nvs_flash_erase(); nvs_flash_init();
                }
                // Read LoRa address from NVS for AP SSID
                uint16_t addr = 0;
                nvs_handle_t h;
                if (nvs_open("lora", NVS_READONLY, &h) == ESP_OK) {
                    nvs_get_u16(h, "my_addr", &addr);
                    nvs_close(h);
                }
                wifi_ota_start(PIN_LED, addr);  // never returns
            }
        }
    }

    // ── 2. NVS init ──
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Load custom sleep/sample intervals
    uint32_t sleep_s = SLEEP_INTERVAL_S;
    uint8_t  samples = SENSOR_SAMPLES;
    {
        nvs_handle_t h;
        if (nvs_open(NVS_NS_SYSTEM, NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u32(h, "sleep_s", &sleep_s);
            nvs_get_u8(h, "samples", &samples);
            nvs_close(h);
        }
    }

    // ── 3. LoRa init ──
    lora_tx_set_firmware_version(FIRMWARE_VERSION);
    lora_tx_init(LORA_UART_NUM, PIN_LORA_TX, PIN_LORA_RX, LORA_BAUD);
    lora_tx_config_t lora_cfg;
    lora_tx_get_config(&lora_cfg);

    // ── 4. Boot window — SKIP on timer wake for battery savings (audit #17) ──
    // Only enter on power-on reset or manual reset, not on deep sleep timer wake.
    bool enter_boot_window = (cause != ESP_SLEEP_WAKEUP_TIMER);

    if (enter_boot_window) {
        ESP_LOGI(TAG, "Boot window (10s). Hold BOOT: 2s+release=PAIR | 5s=WiFi UPDATE");
        uint32_t start_ms  = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t press_start = 0;
        uint32_t held = 0;
        typedef enum { ACT_NONE, ACT_PAIR, ACT_DIAG, ACT_UPDATE } boot_act_t;
        boot_act_t action = ACT_NONE;

        while ((uint32_t)(esp_timer_get_time() / 1000) - start_ms < 10000) {
            if (gpio_get_level(PIN_BUTTON) == 0) {
                if (press_start == 0) press_start = (uint32_t)(esp_timer_get_time() / 1000);
                held = (uint32_t)(esp_timer_get_time() / 1000) - press_start;
                // LED feedback: slow blink < 2s, fast blink 2-5s, solid 5-15s, rapid > 15s
                if (held < 2000)       gpio_set_level(PIN_LED, (esp_timer_get_time() / 500000) % 2);
                else if (held < 5000)  gpio_set_level(PIN_LED, (esp_timer_get_time() / 150000) % 2);
                else if (held < 15000) gpio_set_level(PIN_LED, 1);
                else                   gpio_set_level(PIN_LED, (esp_timer_get_time() / 50000) % 2);
                // Auto-trigger at 15s for factory reset, 5s for update
                if (held >= 15000) { action = ACT_DIAG; break; }  // reuse DIAG as FACTORY_RESET
                if (held >= 5000)  { action = ACT_UPDATE; break; }
            } else if (press_start > 0) {
                // Button released — decide based on hold duration
                if (held >= 2000)      action = ACT_PAIR;
                press_start = 0;
                held = 0;
                if (action != ACT_NONE) break;
            } else {
                gpio_set_level(PIN_LED, (esp_timer_get_time() / 500000) % 2);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        gpio_set_level(PIN_LED, 0);

        if (action == ACT_UPDATE) {
            ESP_LOGI(TAG, "Entering WiFi OTA update mode...");
            led_set_effect(LED_EFFECT_PULSE_CYAN);
            led_flash(PIN_LED, 50, 10);
            wifi_ota_start(PIN_LED, lora_cfg.my_address);  // never returns
        }

        if (action == ACT_DIAG) {
            // Factory reset — erase all NVS (pairing, settings, LoRa config)
            ESP_LOGW(TAG, "FACTORY RESET — erasing NVS...");
            led_set_effect(LED_EFFECT_BLINK_RED);
            led_flash(PIN_LED, 50, 20);
            nvs_flash_erase();
            ESP_LOGW(TAG, "NVS erased. Rebooting...");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();  // never returns
        }

        #define DO_PAIR() do { \
            led_set_effect(LED_EFFECT_BLINK_BLUE); \
            led_flash(PIN_LED, 50, 6); \
            if (lora_tx_enter_pairing()) { \
                led_set_effect(LED_EFFECT_BLINK_GREEN); \
                led_flash(PIN_LED, 500, 3); \
                vTaskDelay(pdMS_TO_TICKS(500)); \
                esp_restart(); \
            } else { \
                led_flash(PIN_LED, 200, 5); \
            } \
            while (gpio_get_level(PIN_BUTTON) == 0) vTaskDelay(pdMS_TO_TICKS(10)); \
            press_start = 0; \
            lora_tx_get_config(&lora_cfg); \
        } while (0)

        if (action == ACT_PAIR) {
            DO_PAIR();
        }

        // Unpaired guard — stay awake blinking until paired
        if (lora_cfg.my_address == 0) {
            ESP_LOGW(TAG, "Unpaired — hold button 2s to pair");
            press_start = 0;
            while (lora_cfg.my_address == 0) {
                led_flash(PIN_LED, 200, 1);
                vTaskDelay(pdMS_TO_TICKS(200));
                if (gpio_get_level(PIN_BUTTON) == 0) {
                    if (press_start == 0) press_start = (uint32_t)(esp_timer_get_time() / 1000);
                    if ((uint32_t)(esp_timer_get_time() / 1000) - press_start >= 2000) {
                        DO_PAIR();
                    }
                } else {
                    press_start = 0;
                }
            }
        }
        #undef DO_PAIR
    } else {
        // Timer wake — check if unpaired (shouldn't happen, but safety guard)
        if (lora_cfg.my_address == 0) {
            ESP_LOGW(TAG, "Unpaired on timer wake — sleeping until manual reset");
            esp_sleep_enable_timer_wakeup((uint64_t)sleep_s * 1000000ULL);
            esp_deep_sleep_start();
        }
    }

    // ── 5. Read sensors ──
    sensor_init(PIN_TRIG, PIN_ECHO);
    int dist_cm = -1;
    sensor_read_cm(&dist_cm);

    power_init(BAT_ADC_CHANNEL, PIN_I2C_SDA, PIN_I2C_SCL);
    power_reading_t pr = {0};
    power_read(&pr);
    int bat_pct = pr.pct;
    uint32_t bat_mv = pr.vbat_mv > 0 ? pr.vbat_mv : 3700;
    float bat_v = (float)bat_mv / 1000.0f;
    ESP_LOGI(TAG, "Power: mode=%s vbat=%lumV (%d%%) I=%ldmA P=%ldmW %s",
             power_mode_str(pr.mode), (unsigned long)pr.vbat_mv, pr.pct,
             (long)pr.current_ma, (long)pr.power_mw,
             pr.charging ? "CHARGING" : "discharging");

    // ── 6. Low battery cutoff (audit #13) ──
    // Disabled for now — ADC reads noise (500-2800mV) without a real battery,
    // which falsely triggers cutoff. TODO: enable when battery is calibrated.
    if (false && bat_mv < BAT_CUTOFF_MV && bat_mv > 2500) {
        ESP_LOGW(TAG, "Low battery %" PRIu32 "mV — extended sleep %ds", bat_mv, BAT_CUTOFF_SLEEP_S);
        led_flash(PIN_LED, 50, 10);  // rapid flash = low battery warning
        esp_sleep_enable_timer_wakeup((uint64_t)BAT_CUTOFF_SLEEP_S * 1000000ULL);
        esp_deep_sleep_start();
    }

    // ── 7. Send TANK packet ──
    s_msg_id++;
    bool acked = lora_tx_send(dist_cm > 0 ? dist_cm : 0, bat_pct, bat_v,
                              power_mode_char(pr.mode),
                              pr.current_ma, pr.power_mw,
                              s_msg_id);

    if (acked) {
        s_ack_failures = 0;
        led_flash(PIN_LED, 80, 2);
        handle_downlink();
    } else {
        s_ack_failures++;
        led_flash(PIN_LED, 200, 3);
    }

    // ── 8. Deep sleep ──
    led_set_effect(LED_EFFECT_NONE);
    led_set(0, LED_OFF); led_set(1, LED_OFF); led_show();
    uint32_t jitter_ms = esp_random() % 2000;
    uint64_t sleep_us  = (uint64_t)sleep_s * 1000000ULL + (uint64_t)jitter_ms * 1000ULL;
    ESP_LOGI(TAG, "Sleeping %" PRIu32 "s (+%" PRIu32 "ms jitter)", sleep_s, jitter_ms);
    esp_sleep_enable_timer_wakeup(sleep_us);
    esp_deep_sleep_start();
}
