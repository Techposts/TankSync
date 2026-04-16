// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

/**
 * led_ws2812 - RMT-based WS2812B driver
 *
 * Uses ESP-IDF 5.x RMT TX channel with the led_strip encoder pattern.
 * WS2812B timing: T0H=350ns, T0L=800ns, T1H=700ns, T1L=600ns, Reset>50us
 * At 10MHz RMT clock: 1 tick = 100ns
 */

#include "led_ws2812.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

static const char *TAG = "led_ws2812";

#define RMT_RESOLUTION_HZ   10000000    // 10 MHz → 100ns per tick
#define MAX_LEDS            8

// WS2812B bit timings in RMT ticks (1 tick = 100ns)
#define T0H  4   // 400ns high for '0'
#define T0L  8   // 800ns low  for '0'
#define T1H  7   // 700ns high for '1'
#define T1L  6   // 600ns low  for '1'
#define RESET_US 80  // >50µs reset

static rmt_channel_handle_t rmt_chan = NULL;
static rmt_encoder_handle_t rmt_enc  = NULL;
static rmt_transmit_config_t tx_cfg = { .loop_count = 0 };

static led_color_t s_leds[MAX_LEDS];
static int         s_num_leds  = 0;
static uint8_t     s_brightness = 50;
static led_effect_t s_current_effect = LED_EFFECT_NONE;
static SemaphoreHandle_t s_led_mutex = NULL;  // protects rmt_transmit + s_leds

// ── Background effect task ───────────────────────────────────────────────────
static void led_effect_task(void *arg) {
    uint32_t step = 0;
    for (;;) {
        if (s_current_effect == LED_EFFECT_NONE) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        switch (s_current_effect) {
            case LED_EFFECT_PULSE_AMBER: {
                // Sinusoidal pulse 10-100% of current brightness
                float val = 0.55f + 0.45f * sinf(step * 0.1f);
                uint8_t base_br = s_brightness;
                // Temporarily adjust brightness for build_rmt_buf inside led_show
                // Note: This is slightly hacky as it modifies global state, but works for single effect
                uint8_t old_br = s_brightness;
                s_brightness = (uint8_t)(old_br * val);
                for(int i=0; i<s_num_leds; i++) s_leds[i] = LED_AMBER;
                led_show();
                s_brightness = old_br;
                vTaskDelay(pdMS_TO_TICKS(30));
                break;
            }
            case LED_EFFECT_BLINK_RED:
                for(int i=0; i<s_num_leds; i++) s_leds[i] = (step % 2) ? LED_RED : LED_OFF;
                led_show();
                vTaskDelay(pdMS_TO_TICKS(250));
                break;
            case LED_EFFECT_BLINK_GREEN:
                for(int i=0; i<s_num_leds; i++) s_leds[i] = (step % 2) ? LED_GREEN : LED_OFF;
                led_show();
                vTaskDelay(pdMS_TO_TICKS(250));
                break;
            default: break;
        }
        step++;
    }
}

// ── Raw RMT symbols buffer: 24 bits × MAX_LEDS + reset ──────────────────────
// Each LED needs 24 rmt_symbol_word_t (one per bit, GRB order)
static rmt_symbol_word_t s_rmt_buf[MAX_LEDS * 24 + 1];

// ── Build the RMT symbol buffer from the LED color array ────────────────────
static void build_rmt_buf(void) {
    int pos = 0;
    for (int led = 0; led < s_num_leds; led++) {
        // WS2812B expects GRB byte order
        uint8_t raw[3] = {
            (uint8_t)(s_leds[led].g * s_brightness / 255),
            (uint8_t)(s_leds[led].r * s_brightness / 255),
            (uint8_t)(s_leds[led].b * s_brightness / 255),
        };
        for (int byte = 0; byte < 3; byte++) {
            for (int bit = 7; bit >= 0; bit--) {
                if ((raw[byte] >> bit) & 1) {
                    s_rmt_buf[pos].level0    = 1;
                    s_rmt_buf[pos].duration0 = T1H;
                    s_rmt_buf[pos].level1    = 0;
                    s_rmt_buf[pos].duration1 = T1L;
                } else {
                    s_rmt_buf[pos].level0    = 1;
                    s_rmt_buf[pos].duration0 = T0H;
                    s_rmt_buf[pos].level1    = 0;
                    s_rmt_buf[pos].duration1 = T0L;
                }
                pos++;
            }
        }
    }
    // Reset pulse: low for >50µs = 500 ticks at 10MHz, split across two durations
    s_rmt_buf[pos].level0    = 0;
    s_rmt_buf[pos].duration0 = 500;
    s_rmt_buf[pos].level1    = 0;
    s_rmt_buf[pos].duration1 = 0;  // marks end of symbols
    pos++;
    (void)pos;
}

esp_err_t led_init(int gpio_num, int num_leds, uint8_t brightness) {
    if (num_leds > MAX_LEDS) num_leds = MAX_LEDS;
    s_num_leds  = num_leds;
    s_brightness = brightness;
    memset(s_leds, 0, sizeof(s_leds));

    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num            = gpio_num,
        .clk_src             = RMT_CLK_SRC_DEFAULT,
        .resolution_hz       = RMT_RESOLUTION_HZ,
        .mem_block_symbols   = 64,
        .trans_queue_depth   = 4,
        .flags.invert_out    = false,
        .flags.with_dma      = false,
    };
    esp_err_t err = rmt_new_tx_channel(&chan_cfg, &rmt_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    // Use simple copy encoder — we supply pre-built RMT symbols
    rmt_copy_encoder_config_t enc_cfg = {};
    err = rmt_new_copy_encoder(&enc_cfg, &rmt_enc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_copy_encoder failed: %s", esp_err_to_name(err));
        return err;
    }

    err = rmt_enable(rmt_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    // Blank all LEDs on init — also tests if LED is actually connected
    build_rmt_buf();
    rmt_transmit(rmt_chan, rmt_enc, s_rmt_buf,
                 (s_num_leds * 24 + 1) * sizeof(rmt_symbol_word_t), &tx_cfg);
    esp_err_t tx_err = rmt_tx_wait_all_done(rmt_chan, pdMS_TO_TICKS(100));

    if (tx_err != ESP_OK) {
        ESP_LOGW(TAG, "WS2812B not connected (RMT flush failed) — LED disabled");
        rmt_disable(rmt_chan);
        rmt_del_channel(rmt_chan);
        rmt_chan = NULL;
        rmt_enc = NULL;
        return ESP_OK;  // Not a fatal error — just no LEDs
    }

    ESP_LOGI(TAG, "WS2812B init OK: gpio=%d leds=%d brightness=%d",
             gpio_num, num_leds, brightness);

    s_led_mutex = xSemaphoreCreateMutex();
    xTaskCreate(led_effect_task, "led_fx", 2048, NULL, 2, NULL);
    return ESP_OK;
}

void led_set_effect(led_effect_t effect) {
    s_current_effect = effect;
    if (effect == LED_EFFECT_NONE) {
        memset(s_leds, 0, sizeof(s_leds));
        led_show();
    }
}

void led_set(int index, led_color_t color) {
    if (index < 0 || index >= s_num_leds) return;
    if (s_led_mutex) xSemaphoreTake(s_led_mutex, portMAX_DELAY);
    s_leds[index] = color;
    if (s_led_mutex) xSemaphoreGive(s_led_mutex);
}

static int s_rmt_fail_count = 0;

esp_err_t led_show(void) {
    if (!rmt_chan || !rmt_enc) return ESP_ERR_INVALID_STATE;
    // Mutex protects concurrent rmt_transmit from led_effect_task + led_task (audit RX#8)
    if (s_led_mutex) xSemaphoreTake(s_led_mutex, portMAX_DELAY);
    if (s_rmt_fail_count > 5) {
        if (s_led_mutex) xSemaphoreGive(s_led_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    build_rmt_buf();
    esp_err_t err = rmt_transmit(rmt_chan, rmt_enc, s_rmt_buf,
                                 (s_num_leds * 24 + 1) * sizeof(rmt_symbol_word_t),
                                 &tx_cfg);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(rmt_chan, pdMS_TO_TICKS(50));
        if (err != ESP_OK) {
            s_rmt_fail_count++;
            if (s_rmt_fail_count == 5)
                ESP_LOGW("led", "RMT failing — LED not connected? Disabling.");
        } else {
            s_rmt_fail_count = 0;
        }
    }
    if (s_led_mutex) xSemaphoreGive(s_led_mutex);
    return err;
}

void led_set_brightness(uint8_t brightness) {
    s_brightness = brightness;
}
