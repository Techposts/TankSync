// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

/**
 * display_sh1106 - SH1106 1.3" OLED driver for ESP-IDF 5.x
 *
 * Uses ESP-IDF 5.x new I2C master API.
 * Full 128×64 framebuffer approach: draw to RAM, flush at 4Hz.
 * Self-contained: includes 6×8 font, no external dependencies.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// ── Font sizes ───────────────────────────────────────────────────────────────
typedef enum {
    FONT_SMALL  = 1,    // 6×8  — status bar text, labels
    FONT_MEDIUM = 2,    // 12×16 — tank names, values
    FONT_LARGE  = 3,    // 18×24 — percentage numbers
} disp_font_t;

// ── Display screens ──────────────────────────────────────────────────────────
typedef enum {
    SCREEN_WATER = 0,        // Per-tank: large tank graphic + water %
    SCREEN_BATTERY,          // Per-tank: battery fill bar + voltage
    SCREEN_SIGNAL,           // Per-tank: LoRa RSSI / SNR
    SCREEN_DIAGNOSTICS,      // All tanks: raw cm + state table
    SCREEN_SYSTEM,           // System: uptime, heap, IP, LoRa stats
    SCREEN_COUNT
} disp_screen_t;

/**
 * Initialize I2C bus and SH1106.
 * @param sda_pin  I2C SDA GPIO
 * @param scl_pin  I2C SCL GPIO
 * @param i2c_addr SH1106 I2C address (usually 0x3C)
 */
esp_err_t disp_init(int sda_pin, int scl_pin, uint8_t i2c_addr);

/** Clear the framebuffer (does not flush to display). */
void disp_clear(void);

/** Flush framebuffer to SH1106. Call after drawing. */
esp_err_t disp_flush(void);

/** Draw a single pixel (x: 0-127, y: 0-63). */
void disp_pixel(int x, int y, bool on);

/** Draw a horizontal line. */
void disp_hline(int x, int y, int width, bool on);

/** Draw a vertical line. */
void disp_vline(int x, int y, int height, bool on);

/** Draw a filled rectangle. */
void disp_fill_rect(int x, int y, int w, int h, bool on);

/** Draw a hollow rectangle. */
void disp_rect(int x, int y, int w, int h, bool on);

/**
 * Draw text at pixel position.
 * @param x     X pixel (0-127)
 * @param y     Y pixel (0-63)
 * @param font  FONT_SMALL / FONT_MEDIUM / FONT_LARGE
 * @param str   Null-terminated ASCII string
 * @return width in pixels of drawn text
 */
int disp_text(int x, int y, disp_font_t font, const char *str);

/** Returns pixel width of string at given font. */
int disp_text_width(disp_font_t font, const char *str);

/**
 * Draw a water tank graphic.
 * @param x, y   Top-left corner
 * @param w, h   Width and height in pixels
 * @param pct    Fill percentage 0-100
 */
void disp_tank_graphic(int x, int y, int w, int h, int pct);

/**
 * Draw a battery graphic.
 * @param x, y   Top-left corner
 * @param w, h   Dimensions
 * @param pct    Fill percentage 0-100
 */
void disp_battery_graphic(int x, int y, int w, int h, int pct);

/**
 * Draw a horizontal progress bar.
 * @param x, y   Top-left
 * @param w, h   Dimensions
 * @param pct    Fill 0-100
 */
void disp_progress_bar(int x, int y, int w, int h, int pct);

/** Set/get current screen for auto-cycling. */
void disp_set_screen(disp_screen_t screen);
disp_screen_t disp_get_screen(void);
void disp_next_screen(void);
