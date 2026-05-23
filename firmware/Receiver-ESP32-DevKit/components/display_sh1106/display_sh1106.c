/**
 * display_sh1106 - SH1106 1.3" OLED driver for ESP-IDF 5.x
 *
 * SH1106 specifics vs SSD1306:
 *   - Internal RAM is 132×64, display shows 128×64 (2px offset per page)
 *   - Page-based write only (8 rows per page = 8 pages total)
 *   - Each write: set page, set column (with +2 offset), send 128 bytes
 *
 * I2C command byte: 0x00 = command stream, 0x40 = data stream
 */

#include "display_sh1106.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "sh1106";

// ── Framebuffer: 128×64 = 1024 bytes (1 bit per pixel) ─────────────────────
// Organization: fb[page][col], page=0..7 (top to bottom), col=0..127
// bit 0 = top of page, bit 7 = bottom of page
static uint8_t s_fb[8][128];
static i2c_master_dev_handle_t s_dev = NULL;

// SH1106 column offset (2 pixels for 132-wide internal RAM)
#define SH1106_COL_OFFSET   2

// ── 6×8 monospace font (ASCII 32-127) ──────────────────────────────────────
// Each character: 6 bytes (columns 0-5), 8 rows per column (LSB=top)
// Source: classic embedded 6×8 font
static const uint8_t FONT6x8[][6] = {
    {0x00,0x00,0x00,0x00,0x00,0x00}, // 32 ' '
    {0x00,0x00,0x5F,0x00,0x00,0x00}, // 33 '!'
    {0x00,0x07,0x00,0x07,0x00,0x00}, // 34 '"'
    {0x14,0x7F,0x14,0x7F,0x14,0x00}, // 35 '#'
    {0x24,0x2A,0x7F,0x2A,0x12,0x00}, // 36 '$'
    {0x23,0x13,0x08,0x64,0x62,0x00}, // 37 '%'
    {0x36,0x49,0x55,0x22,0x50,0x00}, // 38 '&'
    {0x00,0x05,0x03,0x00,0x00,0x00}, // 39 '\''
    {0x00,0x1C,0x22,0x41,0x00,0x00}, // 40 '('
    {0x00,0x41,0x22,0x1C,0x00,0x00}, // 41 ')'
    {0x08,0x2A,0x1C,0x2A,0x08,0x00}, // 42 '*'
    {0x08,0x08,0x3E,0x08,0x08,0x00}, // 43 '+'
    {0x00,0x50,0x30,0x00,0x00,0x00}, // 44 ','
    {0x08,0x08,0x08,0x08,0x08,0x00}, // 45 '-'
    {0x00,0x60,0x60,0x00,0x00,0x00}, // 46 '.'
    {0x20,0x10,0x08,0x04,0x02,0x00}, // 47 '/'
    {0x3E,0x51,0x49,0x45,0x3E,0x00}, // 48 '0'
    {0x00,0x42,0x7F,0x40,0x00,0x00}, // 49 '1'
    {0x42,0x61,0x51,0x49,0x46,0x00}, // 50 '2'
    {0x21,0x41,0x45,0x4B,0x31,0x00}, // 51 '3'
    {0x18,0x14,0x12,0x7F,0x10,0x00}, // 52 '4'
    {0x27,0x45,0x45,0x45,0x39,0x00}, // 53 '5'
    {0x3C,0x4A,0x49,0x49,0x30,0x00}, // 54 '6'
    {0x03,0x01,0x71,0x09,0x07,0x00}, // 55 '7'
    {0x36,0x49,0x49,0x49,0x36,0x00}, // 56 '8'
    {0x06,0x49,0x49,0x29,0x1E,0x00}, // 57 '9'
    {0x00,0x36,0x36,0x00,0x00,0x00}, // 58 ':'
    {0x00,0x56,0x36,0x00,0x00,0x00}, // 59 ';'
    {0x00,0x08,0x14,0x22,0x41,0x00}, // 60 '<'
    {0x14,0x14,0x14,0x14,0x14,0x00}, // 61 '='
    {0x41,0x22,0x14,0x08,0x00,0x00}, // 62 '>'
    {0x02,0x01,0x51,0x09,0x06,0x00}, // 63 '?'
    {0x32,0x49,0x79,0x41,0x3E,0x00}, // 64 '@'
    {0x7E,0x11,0x11,0x11,0x7E,0x00}, // 65 'A'
    {0x7F,0x49,0x49,0x49,0x36,0x00}, // 66 'B'
    {0x3E,0x41,0x41,0x41,0x22,0x00}, // 67 'C'
    {0x7F,0x41,0x41,0x22,0x1C,0x00}, // 68 'D'
    {0x7F,0x49,0x49,0x49,0x41,0x00}, // 69 'E'
    {0x7F,0x09,0x09,0x01,0x01,0x00}, // 70 'F'
    {0x3E,0x41,0x41,0x51,0x32,0x00}, // 71 'G'
    {0x7F,0x08,0x08,0x08,0x7F,0x00}, // 72 'H'
    {0x00,0x41,0x7F,0x41,0x00,0x00}, // 73 'I'
    {0x20,0x40,0x41,0x3F,0x01,0x00}, // 74 'J'
    {0x7F,0x08,0x14,0x22,0x41,0x00}, // 75 'K'
    {0x7F,0x40,0x40,0x40,0x40,0x00}, // 76 'L'
    {0x7F,0x02,0x04,0x02,0x7F,0x00}, // 77 'M'
    {0x7F,0x04,0x08,0x10,0x7F,0x00}, // 78 'N'
    {0x3E,0x41,0x41,0x41,0x3E,0x00}, // 79 'O'
    {0x7F,0x09,0x09,0x09,0x06,0x00}, // 80 'P'
    {0x3E,0x41,0x51,0x21,0x5E,0x00}, // 81 'Q'
    {0x7F,0x09,0x19,0x29,0x46,0x00}, // 82 'R'
    {0x46,0x49,0x49,0x49,0x31,0x00}, // 83 'S'
    {0x01,0x01,0x7F,0x01,0x01,0x00}, // 84 'T'
    {0x3F,0x40,0x40,0x40,0x3F,0x00}, // 85 'U'
    {0x1F,0x20,0x40,0x20,0x1F,0x00}, // 86 'V'
    {0x7F,0x20,0x18,0x20,0x7F,0x00}, // 87 'W'
    {0x63,0x14,0x08,0x14,0x63,0x00}, // 88 'X'
    {0x03,0x04,0x78,0x04,0x03,0x00}, // 89 'Y'
    {0x61,0x51,0x49,0x45,0x43,0x00}, // 90 'Z'
    {0x00,0x00,0x7F,0x41,0x41,0x00}, // 91 '['
    {0x02,0x04,0x08,0x10,0x20,0x00}, // 92 '\'
    {0x41,0x41,0x7F,0x00,0x00,0x00}, // 93 ']'
    {0x04,0x02,0x01,0x02,0x04,0x00}, // 94 '^'
    {0x40,0x40,0x40,0x40,0x40,0x00}, // 95 '_'
    {0x00,0x01,0x02,0x04,0x00,0x00}, // 96 '`'
    {0x20,0x54,0x54,0x54,0x78,0x00}, // 97 'a'
    {0x7F,0x48,0x44,0x44,0x38,0x00}, // 98 'b'
    {0x38,0x44,0x44,0x44,0x20,0x00}, // 99 'c'
    {0x38,0x44,0x44,0x48,0x7F,0x00}, // 100 'd'
    {0x38,0x54,0x54,0x54,0x18,0x00}, // 101 'e'
    {0x08,0x7E,0x09,0x01,0x02,0x00}, // 102 'f'
    {0x08,0x54,0x54,0x54,0x3C,0x00}, // 103 'g'
    {0x7F,0x08,0x04,0x04,0x78,0x00}, // 104 'h'
    {0x00,0x44,0x7D,0x40,0x00,0x00}, // 105 'i'
    {0x20,0x40,0x44,0x3D,0x00,0x00}, // 106 'j'
    {0x00,0x7F,0x10,0x28,0x44,0x00}, // 107 'k'
    {0x00,0x41,0x7F,0x40,0x00,0x00}, // 108 'l'
    {0x7C,0x04,0x18,0x04,0x78,0x00}, // 109 'm'
    {0x7C,0x08,0x04,0x04,0x78,0x00}, // 110 'n'
    {0x38,0x44,0x44,0x44,0x38,0x00}, // 111 'o'
    {0x7C,0x14,0x14,0x14,0x08,0x00}, // 112 'p'
    {0x08,0x14,0x14,0x18,0x7C,0x00}, // 113 'q'
    {0x7C,0x08,0x04,0x04,0x08,0x00}, // 114 'r'
    {0x48,0x54,0x54,0x54,0x20,0x00}, // 115 's'
    {0x04,0x3F,0x44,0x40,0x20,0x00}, // 116 't'
    {0x3C,0x40,0x40,0x20,0x7C,0x00}, // 117 'u'
    {0x1C,0x20,0x40,0x20,0x1C,0x00}, // 118 'v'
    {0x3C,0x40,0x30,0x40,0x3C,0x00}, // 119 'w'
    {0x44,0x28,0x10,0x28,0x44,0x00}, // 120 'x'
    {0x0C,0x50,0x50,0x50,0x3C,0x00}, // 121 'y'
    {0x44,0x64,0x54,0x4C,0x44,0x00}, // 122 'z'
    {0x00,0x08,0x36,0x41,0x00,0x00}, // 123 '{'
    {0x00,0x00,0x7F,0x00,0x00,0x00}, // 124 '|'
    {0x00,0x41,0x36,0x08,0x00,0x00}, // 125 '}'
    {0x08,0x04,0x08,0x10,0x08,0x00}, // 126 '~'
    {0x00,0x00,0x00,0x00,0x00,0x00}, // 127 DEL
};

// ── I2C helpers ──────────────────────────────────────────────────────────────

static esp_err_t sh1106_write_cmd(uint8_t cmd) {
    uint8_t buf[2] = {0x00, cmd};
    return i2c_master_transmit(s_dev, buf, 2, pdMS_TO_TICKS(20));
}

static esp_err_t sh1106_write_data(const uint8_t *data, size_t len) {
    // Static buffer avoids alloca stack overflow risk (audit RX#6)
    static uint8_t buf[129];  // 128 data + 1 prefix
    if (len > 128) len = 128;
    buf[0] = 0x40;  // data stream indicator
    memcpy(buf + 1, data, len);
    // 129 bytes at 100kHz = ~12ms. Use 25ms timeout.
    return i2c_master_transmit(s_dev, buf, len + 1, pdMS_TO_TICKS(25));
}

// ── SH1106 initialization sequence ──────────────────────────────────────────
static const uint8_t SH1106_INIT[] = {
    0xAE,       // Display off
    0xD5,0x80,  // Set display clock div
    0xA8,0x3F,  // Multiplex ratio 64
    0xD3,0x00,  // Display offset = 0
    0x40,       // Display start line = 0
    0xAD,0x8B,  // Internal DC-DC on
    0xA1,       // Segment re-map (col 127 → SEG0)
    0xC8,       // COM scan direction reversed
    0xDA,0x12,  // COM pins alternative
    0x81,0xCF,  // Contrast
    0xD9,0xF1,  // Pre-charge period
    0xDB,0x40,  // VCOMH deselect level
    0xA6,       // Normal display (not inverted)
    0xA4,       // Entire display on (use RAM)
    0x20,       // Page addressing mode
    0xAF,       // Display on
};

// ── Public API ───────────────────────────────────────────────────────────────

static disp_screen_t s_current_screen = SCREEN_WATER;
static i2c_master_bus_handle_t s_bus = NULL;

esp_err_t disp_init(int sda_pin, int scl_pin, uint8_t i2c_addr) {
    i2c_master_bus_config_t bus_cfg = {
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .i2c_port          = I2C_NUM_0,
        .scl_io_num        = scl_pin,
        .sda_io_num        = sda_pin,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed: %s — OLED disabled", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = i2c_addr,
        .scl_speed_hz    = 100000,   // 100kHz — stable; 400kHz glitches during LoRa TX
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C device add failed: %s — OLED disabled", esp_err_to_name(err));
        return err;
    }

    // Send init sequence — if OLED not connected, first cmd will fail
    for (size_t i = 0; i < sizeof(SH1106_INIT); i++) {
        err = sh1106_write_cmd(SH1106_INIT[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "OLED not responding (cmd 0x%02X) — display disabled", SH1106_INIT[i]);
            return err;
        }
    }

    memset(s_fb, 0, sizeof(s_fb));
    disp_flush();

    ESP_LOGI(TAG, "SH1106 init OK (sda=%d scl=%d addr=0x%02X)", sda_pin, scl_pin, i2c_addr);
    return ESP_OK;
}

void disp_clear(void) {
    memset(s_fb, 0, sizeof(s_fb));
}

static int s_i2c_fail_count = 0;

esp_err_t disp_flush(void) {
    // If I2C has failed repeatedly, stop trying (OLED not connected) (audit RX#7)
    if (s_i2c_fail_count > 0) return ESP_ERR_INVALID_STATE;  // immediate disable on first I2C error

    for (int page = 0; page < 8; page++) {
        esp_err_t err;
        err = sh1106_write_cmd(0xB0 | page);
        if (err != ESP_OK) goto i2c_fail;
        sh1106_write_cmd(0x02);
        sh1106_write_cmd(0x10);
        err = sh1106_write_data(s_fb[page], 128);
        if (err != ESP_OK) goto i2c_fail;
    }
    s_i2c_fail_count = 0;  // reset on success
    return ESP_OK;

i2c_fail:
    s_i2c_fail_count++;
    if (s_i2c_fail_count == 3)
        ESP_LOGW(TAG, "I2C failing — OLED not connected? Display disabled.");
    return ESP_ERR_TIMEOUT;
}

void disp_pixel(int x, int y, bool on) {
    if (x < 0 || x > 127 || y < 0 || y > 63) return;
    int page = y >> 3;        // y / 8
    int bit  = y & 0x07;      // y % 8
    if (on) s_fb[page][x] |=  (1 << bit);
    else    s_fb[page][x] &= ~(1 << bit);
}

void disp_hline(int x, int y, int width, bool on) {
    for (int i = 0; i < width; i++) disp_pixel(x + i, y, on);
}

void disp_vline(int x, int y, int height, bool on) {
    for (int i = 0; i < height; i++) disp_pixel(x, y + i, on);
}

void disp_fill_rect(int x, int y, int w, int h, bool on) {
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            disp_pixel(col, row, on);
}

void disp_rect(int x, int y, int w, int h, bool on) {
    disp_hline(x,       y,       w, on);
    disp_hline(x,       y+h-1,   w, on);
    disp_vline(x,       y,       h, on);
    disp_vline(x+w-1,   y,       h, on);
}

// ── Text rendering ───────────────────────────────────────────────────────────
// Scale factor n: each font pixel → n×n block of display pixels

static void draw_char_scaled(int x, int y, char c, int scale) {
    if (c < 32 || c > 127) c = '?';
    const uint8_t *col_data = FONT6x8[c - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t col_bits = col_data[col];
        for (int row = 0; row < 8; row++) {
            bool pixel = (col_bits >> row) & 1;
            // Draw scale×scale block for this pixel
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    disp_pixel(x + col * scale + sx, y + row * scale + sy, pixel);
        }
    }
}

int disp_text(int x, int y, disp_font_t font, const char *str) {
    int scale = (int)font;
    int char_w = 6 * scale;  // 5px + 1px spacing, scaled
    int cx = x;
    while (*str) {
        draw_char_scaled(cx, y, *str, scale);
        cx += char_w;
        str++;
    }
    return cx - x;
}

int disp_text_width(disp_font_t font, const char *str) {
    return (int)(6 * (int)font * strlen(str));
}

// ── Graphics primitives ──────────────────────────────────────────────────────

void disp_tank_graphic(int x, int y, int w, int h, int pct) {
    // Outer rectangle
    disp_rect(x, y, w, h, true);
    // Cap (filler neck)
    int cap_w = w / 2, cap_h = 3;
    disp_fill_rect(x + (w - cap_w) / 2, y - cap_h, cap_w, cap_h, true);
    // Water fill (from bottom)
    if (pct > 0 && pct <= 100) {
        int water_h = (int)((h - 2) * pct / 100);
        int water_y = y + h - 1 - water_h;
        disp_fill_rect(x + 1, water_y, w - 2, water_h, true);
    }
}

// Vertical capsule outline + proportional water fill. No lid — entire glyph
// stays inside the [x..x+w-1, y..y+h-1] box. Top and bottom rows are indented
// 2 px on each side to suggest rounded corners on the 1-bit display.
void disp_tank_capsule(int x, int y, int w, int h, int pct) {
    if (h < 6 || w < 6) {  // too small for rounding — fall back to plain rect
        disp_rect(x, y, w, h, true);
        if (pct > 0) {
            int water_h = (int)((h - 2) * pct / 100);
            int water_y = y + h - 1 - water_h;
            disp_fill_rect(x + 1, water_y, w - 2, water_h, true);
        }
        return;
    }
    // Top arc: y row indented 2 px, y+1 row indented 1 px on each side
    disp_hline(x + 2, y,         w - 4, true);
    disp_pixel(x + 1,     y + 1, true);
    disp_pixel(x + w - 2, y + 1, true);
    // Sides — full vertical between the rounded corners
    disp_vline(x,         y + 2, h - 4, true);
    disp_vline(x + w - 1, y + 2, h - 4, true);
    // Bottom arc: mirror of top
    disp_pixel(x + 1,     y + h - 2, true);
    disp_pixel(x + w - 2, y + h - 2, true);
    disp_hline(x + 2, y + h - 1, w - 4, true);
    // Water fill — proportional inside the inner box (4 px reserved for rounding).
    if (pct > 0) {
        int max_h = h - 4;
        if (max_h < 1) max_h = 1;
        int water_h = (max_h * pct) / 100;
        if (water_h > max_h) water_h = max_h;
        if (water_h > 0) {
            int water_y = y + h - 2 - water_h;
            disp_fill_rect(x + 1, water_y, w - 2, water_h, true);
        }
    }
}

void disp_battery_graphic(int x, int y, int w, int h, int pct) {
    disp_rect(x, y, w, h, true);
    // Terminal nub
    disp_fill_rect(x + w, y + h/4, 3, h/2, true);
    if (pct > 0) {
        int fill_w = (int)((w - 2) * pct / 100);
        disp_fill_rect(x + 1, y + 1, fill_w, h - 2, true);
    }
}

void disp_progress_bar(int x, int y, int w, int h, int pct) {
    disp_rect(x, y, w, h, true);
    if (pct > 0) {
        int fill_w = (int)((w - 2) * pct / 100);
        disp_fill_rect(x + 1, y + 1, fill_w, h - 2, true);
    }
}

// ── Screen cycling ───────────────────────────────────────────────────────────

void disp_set_screen(disp_screen_t screen) {
    s_current_screen = screen;
}

disp_screen_t disp_get_screen(void) {
    return s_current_screen;
}

void disp_next_screen(void) {
    s_current_screen = (disp_screen_t)((s_current_screen + 1) % SCREEN_COUNT);
}
