/**
 * sensor_sr04 implementation
 *
 * Timing: uses esp_timer_get_time() for µs-resolution echo measurement.
 * GPIO: TRIG = output, ECHO = input (no interrupts, busy-wait for max 30ms).
 *
 * Robust filtering algorithm:
 *   1. Collect N samples
 *   2. Sort
 *   3. Compute Q1 (25th percentile) and Q3 (75th percentile)
 *   4. IQR = Q3 - Q1
 *   5. Reject samples outside [Q1 - 1.5*IQR, Q3 + 1.5*IQR]
 *   6. Return median of remaining samples
 *   7. If fewer than 2 samples remain → error
 */

#include "sensor_sr04.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "sensor";

static int s_trig_pin = -1;
static int s_echo_pin = -1;

// ── Sorting helper (insertion sort — fine for N=5..10) ────────────────────────
static void sort_ints(int *arr, int n) {
    for (int i = 1; i < n; i++) {
        int key = arr[i], j = i - 1;
        while (j >= 0 && arr[j] > key) { arr[j+1] = arr[j]; j--; }
        arr[j+1] = key;
    }
}

// ── Single trigger-echo measurement ──────────────────────────────────────────
esp_err_t sensor_read_single_cm(int *out_cm) {
    *out_cm = -1;

    // Trigger: 10µs HIGH pulse
    gpio_set_level(s_trig_pin, 0);
    esp_rom_delay_us(2);
    gpio_set_level(s_trig_pin, 1);
    esp_rom_delay_us(SENSOR_TRIG_US);
    gpio_set_level(s_trig_pin, 0);

    // Wait for ECHO HIGH. The hardcoded 5ms here was tuned for HC-SR04 /
    // JSN-SR04T (which respond in <500µs) but JSN-SR04M / AJ-SR04M variants
    // take 10–15ms before asserting ECHO — they ran an internal settling
    // cycle first. Use SENSOR_ECHO_WAIT_US (30ms default) so both variants
    // work. See sensor_sr04.h for the lineage.
    int64_t t_start = esp_timer_get_time();
    while (!gpio_get_level(s_echo_pin)) {
        if (esp_timer_get_time() - t_start > SENSOR_ECHO_WAIT_US) {
            ESP_LOGD(TAG, "Echo start timeout (%d us)", SENSOR_ECHO_WAIT_US);
            return ESP_ERR_TIMEOUT;
        }
    }

    // Measure ECHO pulse width
    int64_t t_echo_start = esp_timer_get_time();
    while (gpio_get_level(s_echo_pin)) {
        if (esp_timer_get_time() - t_echo_start > SENSOR_TIMEOUT_US) {
            ESP_LOGD(TAG, "Echo too long (>5m)");
            *out_cm = -1;
            return ESP_ERR_TIMEOUT;
        }
    }
    int64_t echo_us = esp_timer_get_time() - t_echo_start;

    int cm = (int)(echo_us / SENSOR_US_TO_CM);
    ESP_LOGD(TAG, "Echo: %lldµs → %dcm", (long long)echo_us, cm);
    *out_cm = cm;
    return ESP_OK;
}

// ── Robust multi-sample read with IQR outlier rejection ──────────────────────
esp_err_t sensor_read_cm(int *out_cm) {
    int samples[SENSOR_SAMPLES];
    int valid_count = 0;

    for (int i = 0; i < SENSOR_SAMPLES; i++) {
        int cm = -1;
        esp_err_t err = sensor_read_single_cm(&cm);
        if (err == ESP_OK && cm >= SENSOR_MIN_CM && cm <= SENSOR_MAX_CM) {
            samples[valid_count++] = cm;
        } else {
            ESP_LOGD(TAG, "Sample %d invalid: %dcm", i, cm);
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_SAMPLE_DELAY_MS));
    }

    ESP_LOGI(TAG, "%d/%d valid samples", valid_count, SENSOR_SAMPLES);

    if (valid_count < 2) {
        ESP_LOGW(TAG, "Not enough valid samples");
        return ESP_ERR_INVALID_RESPONSE;
    }

    sort_ints(samples, valid_count);

    // IQR filter
    int q1_idx  = valid_count / 4;
    int q3_idx  = (valid_count * 3) / 4;
    int q1      = samples[q1_idx];
    int q3      = samples[q3_idx];
    int iqr     = q3 - q1;

    // Fences: 1.5 * IQR (standard Tukey fence)
    int lower = q1 - (iqr * 3 / 2);
    int upper = q3 + (iqr * 3 / 2);

    // If IQR is very small (all samples agree), use the tight range
    if (iqr < 2) {
        lower = q1 - 5;
        upper = q3 + 5;
    }

    int filtered[SENSOR_SAMPLES];
    int fcount = 0;
    for (int i = 0; i < valid_count; i++) {
        if (samples[i] >= lower && samples[i] <= upper) {
            filtered[fcount++] = samples[i];
        } else {
            ESP_LOGD(TAG, "Rejected outlier: %dcm (fence [%d, %d])",
                     samples[i], lower, upper);
        }
    }

    if (fcount == 0) {
        // All filtered — fall back to median of valid samples
        ESP_LOGW(TAG, "All samples filtered, using raw median");
        *out_cm = samples[valid_count / 2];
        return ESP_OK;
    }

    // Median of filtered samples
    *out_cm = filtered[fcount / 2];
    ESP_LOGI(TAG, "Final reading: %dcm (from %d filtered samples)", *out_cm, fcount);
    return ESP_OK;
}

// ── Init ──────────────────────────────────────────────────────────────────────
esp_err_t sensor_init(int trig_pin, int echo_pin) {
    s_trig_pin = trig_pin;
    s_echo_pin = echo_pin;

    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << trig_pin),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_cfg);
    gpio_set_level(trig_pin, 0);

    io_cfg.pin_bit_mask = (1ULL << echo_pin);
    io_cfg.mode         = GPIO_MODE_INPUT;
    gpio_config(&io_cfg);

    ESP_LOGI(TAG, "Init: TRIG=GPIO%d ECHO=GPIO%d", trig_pin, echo_pin);
    return ESP_OK;
}
