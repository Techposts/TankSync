/**
 * sensor_ld2413 — see sensor_ld2413.h for protocol + status notes.
 */

#include "sensor_ld2413.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ld2413";

// Frame structure constants (datasheet sec 5.3)
#define LD2413_BAUD             115200
#define LD2413_FRAME_LEN        14
#define LD2413_HDR_LEN          4
#define LD2413_TAIL_LEN         4
#define LD2413_PAYLOAD_LEN      6  // 2-byte length field + 4-byte float32

static const uint8_t LD2413_HDR[LD2413_HDR_LEN]   = {0xF4, 0xF3, 0xF2, 0xF1};
static const uint8_t LD2413_TAIL[LD2413_TAIL_LEN] = {0xF8, 0xF7, 0xF6, 0xF5};

#define LD2413_UART_RX_BUF      512
#define LD2413_UART_TX_BUF      0       // We only read; no TX in factory-stock mode

static uart_port_t s_uart = UART_NUM_MAX;     // Sentinel: not initialized
static bool        s_initialized = false;

// ── Init / Deinit ────────────────────────────────────────────────────────────

esp_err_t ld2413_init(uart_port_t uart_num, int pin_tx, int pin_rx) {
    s_uart = uart_num;

    const uart_config_t cfg = {
        .baud_rate  = LD2413_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // If a previous init left the driver installed (e.g. wake-cycle replay
    // without full reset), tear it down first so uart_driver_install doesn't
    // return ESP_ERR_INVALID_STATE.
    if (uart_is_driver_installed(s_uart)) {
        uart_driver_delete(s_uart);
    }

    esp_err_t err = uart_driver_install(s_uart, LD2413_UART_RX_BUF, LD2413_UART_TX_BUF, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(s_uart, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_set_pin(s_uart, pin_tx, pin_rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "LD2413 UART%d ready (TX=GPIO%d RX=GPIO%d, %d baud) — DRIVER UNTESTED ON HARDWARE",
             (int)s_uart, pin_tx, pin_rx, LD2413_BAUD);
    return ESP_OK;
}

void ld2413_deinit(void) {
    if (s_initialized && uart_is_driver_installed(s_uart)) {
        uart_driver_delete(s_uart);
    }
    s_initialized = false;
}

// ── Frame scanner ────────────────────────────────────────────────────────────

// Read one byte with a deadline. Returns -1 if deadline passed before a byte
// was available.
static int read_byte_deadline(int64_t deadline_us) {
    uint8_t b;
    while (esp_timer_get_time() < deadline_us) {
        int n = uart_read_bytes(s_uart, &b, 1, pdMS_TO_TICKS(20));
        if (n > 0) return (int)b;
    }
    return -1;
}

// Scan for the 4-byte header pattern. Returns ESP_OK if header found before
// deadline, ESP_ERR_TIMEOUT otherwise.
static esp_err_t scan_for_header(int64_t deadline_us) {
    int match = 0;
    while (match < LD2413_HDR_LEN) {
        int b = read_byte_deadline(deadline_us);
        if (b < 0) return ESP_ERR_TIMEOUT;
        if (b == LD2413_HDR[match]) {
            match++;
        } else if (b == LD2413_HDR[0]) {
            // False start — current byte still matches first header byte
            match = 1;
        } else {
            match = 0;
        }
    }
    return ESP_OK;
}

// Read one complete report frame (header already consumed) and extract the
// distance value. Returns ESP_OK on a valid frame, error otherwise.
static esp_err_t read_one_frame(int *out_cm, int64_t deadline_us) {
    esp_err_t err = scan_for_header(deadline_us);
    if (err != ESP_OK) return err;

    // Read the 10 remaining bytes: 2-byte length + 4-byte float + 4-byte tail
    uint8_t payload[LD2413_PAYLOAD_LEN + LD2413_TAIL_LEN];
    for (int i = 0; i < (int)sizeof(payload); i++) {
        int b = read_byte_deadline(deadline_us);
        if (b < 0) return ESP_ERR_TIMEOUT;
        payload[i] = (uint8_t)b;
    }

    // Validate tail
    if (memcmp(&payload[LD2413_PAYLOAD_LEN], LD2413_TAIL, LD2413_TAIL_LEN) != 0) {
        ESP_LOGD(TAG, "Tail mismatch — discarding frame");
        return ESP_ERR_INVALID_CRC;
    }

    // Length field should be 0x0004 (4-byte float payload). If it's something
    // else, the firmware may have been reconfigured; warn but keep parsing
    // assuming float32.
    uint16_t length = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    if (length != 4) {
        ESP_LOGW(TAG, "Unexpected length field 0x%04x (expected 0x0004)", length);
    }

    // Extract float32 LE — ESP32-C3 is little-endian, direct memcpy works
    float dist_mm;
    memcpy(&dist_mm, &payload[2], sizeof(float));

    // Sanity check: NaN/Inf can happen if the radar hasn't seen a target yet
    if (dist_mm != dist_mm || dist_mm < 0.0f || dist_mm > 12000.0f) {
        ESP_LOGD(TAG, "Distance out of plausible range: %.2f mm", dist_mm);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int cm = (int)((dist_mm + 5.0f) / 10.0f);  // round-half-up mm → cm
    if (cm < LD2413_MIN_CM || cm > LD2413_MAX_CM) {
        ESP_LOGD(TAG, "Distance %d cm outside sensor envelope (%d-%d)", cm, LD2413_MIN_CM, LD2413_MAX_CM);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_cm = cm;
    return ESP_OK;
}

// ── IQR-median filter (mirrors sensor_sr04's pipeline) ──────────────────────

static int cmp_int(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

static int iqr_median(int *samples, int n) {
    qsort(samples, n, sizeof(int), cmp_int);
    if (n <= 3) return samples[n / 2];
    // Drop top + bottom quartile, take median of the middle 50 %
    int q = n / 4;
    int lo = q;
    int hi = n - q - 1;
    return samples[(lo + hi) / 2];
}

// ── Public read ─────────────────────────────────────────────────────────────

esp_err_t ld2413_read_cm(int *out_cm) {
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!out_cm)        return ESP_ERR_INVALID_ARG;

    int samples[LD2413_SAMPLES];
    int n_samples = 0;
    int n_attempts = 0;
    int64_t deadline_us = esp_timer_get_time() + (LD2413_READ_TIMEOUT_MS * 1000LL);

    // Drain any stale frames buffered while we were doing other boot work
    uart_flush_input(s_uart);

    while (n_samples < LD2413_SAMPLES && esp_timer_get_time() < deadline_us) {
        int cm = -1;
        esp_err_t err = read_one_frame(&cm, deadline_us);
        n_attempts++;
        if (err == ESP_OK) {
            samples[n_samples++] = cm;
        }
        // Other errors (timeout / CRC / invalid range) just retry next frame
    }

    if (n_samples < 3) {
        ESP_LOGW(TAG, "Only %d/%d valid frames in %dms (%d attempts)",
                 n_samples, LD2413_SAMPLES, LD2413_READ_TIMEOUT_MS, n_attempts);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_cm = iqr_median(samples, n_samples);
    ESP_LOGI(TAG, "Read: %d cm (median of %d frames, %d attempts)",
             *out_cm, n_samples, n_attempts);
    return ESP_OK;
}
