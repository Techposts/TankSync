// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

/**
 * led_ws2812 - RMT-based WS2812B driver for ESP-IDF 5.x
 * Supports up to 8 LEDs. Thread-safe color setting.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct { uint8_t r, g, b; } led_color_t;

// Named colors
#define LED_OFF         ((led_color_t){0,   0,   0  })
#define LED_RED         ((led_color_t){255, 0,   0  })
#define LED_GREEN       ((led_color_t){0,   255, 0  })
#define LED_BLUE        ((led_color_t){0,   0,   255})
#define LED_YELLOW      ((led_color_t){255, 200, 0  })
#define LED_CYAN        ((led_color_t){0,   255, 200})
#define LED_ORANGE      ((led_color_t){255, 100, 0  })
#define LED_MAGENTA     ((led_color_t){200, 0,   255})
#define LED_WHITE       ((led_color_t){200, 200, 200})
#define LED_AMBER       ((led_color_t){255, 176, 0  })

typedef enum {
    LED_EFFECT_NONE = 0,
    LED_EFFECT_PULSE_AMBER,   // Terminal theme pulse
    LED_EFFECT_BLINK_RED,     // Error / factory reset
    LED_EFFECT_BLINK_GREEN,   // Success
    LED_EFFECT_BLINK_BLUE,    // Pairing mode
    LED_EFFECT_PULSE_CYAN,    // WiFi AP mode
} led_effect_t;

/**
 * Initialize WS2812B strip via RMT peripheral.
 * @param gpio_num  Data pin
 * @param num_leds  Number of LEDs (max 8)
 * @param brightness  Global brightness 0-255
 */
esp_err_t led_init(int gpio_num, int num_leds, uint8_t brightness);

/**
 * Set a single LED color. Call led_show() to apply.
 */
void led_set(int index, led_color_t color);

/**
 * Apply all pending color changes to the strip.
 */
esp_err_t led_show(void);

/**
 * Set brightness and refresh. 0=off, 255=full.
 */
void led_set_brightness(uint8_t brightness);

/**
 * Set a background visual effect.
 */
void led_set_effect(led_effect_t effect);
