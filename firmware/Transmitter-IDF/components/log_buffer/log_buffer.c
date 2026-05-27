/**
 * log_buffer — see log_buffer.h for design notes.
 *
 * Implementation: byte-based circular buffer protected by a FreeRTOS mutex.
 * Each ESP_LOG call routes through esp_log_set_vprintf(), where we vsnprintf
 * the formatted line into a small stack buffer, copy bytes into the ring,
 * and forward the same bytes to the original vprintf so the USB-Serial-JTAG
 * console still works.
 */

#include "log_buffer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char           s_ring[LOG_BUFFER_BYTES];
static size_t         s_head     = 0;                  // next write index
static size_t         s_total    = 0;                  // monotonic byte count
static SemaphoreHandle_t s_mu    = NULL;
static vprintf_like_t s_prev_vprintf = NULL;
static bool           s_inited   = false;

static int log_buffer_vprintf(const char *fmt, va_list args) {
    // Format into a local stack buffer first so we hold the mutex for the
    // minimum time. 256 chars handles every ESP_LOGI / LOGW / LOGE the
    // codebase emits without truncation in practice.
    char line[256];
    va_list args_copy;
    va_copy(args_copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, args_copy);
    va_end(args_copy);
    if (n < 0) n = 0;
    if (n >= (int)sizeof(line)) n = sizeof(line) - 1;

    if (s_mu && xSemaphoreTake(s_mu, pdMS_TO_TICKS(5)) == pdTRUE) {
        for (int i = 0; i < n; i++) {
            s_ring[s_head] = line[i];
            s_head = (s_head + 1) % sizeof(s_ring);
            s_total++;
        }
        xSemaphoreGive(s_mu);
    }

    // Forward to the original sink (USB-Serial-JTAG / UART console) so the
    // serial monitor still works when a cable is attached.
    if (s_prev_vprintf) return s_prev_vprintf(fmt, args);
    return vprintf(fmt, args);
}

esp_err_t log_buffer_init(void) {
    if (s_inited) return ESP_OK;
    s_mu = xSemaphoreCreateMutex();
    if (!s_mu) return ESP_ERR_NO_MEM;
    s_prev_vprintf = esp_log_set_vprintf(log_buffer_vprintf);
    s_inited = true;
    return ESP_OK;
}

size_t log_buffer_read(char *out_buf, size_t out_sz, size_t *cursor) {
    if (!out_buf || out_sz < 2 || !cursor) return 0;
    if (!s_mu) { out_buf[0] = '\0'; return 0; }

    size_t written = 0;
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) != pdTRUE) {
        out_buf[0] = '\0';
        return 0;
    }

    size_t total  = s_total;
    size_t oldest = (total > sizeof(s_ring)) ? (total - sizeof(s_ring)) : 0;

    // Detect and report gap for clients that fell behind.
    if (*cursor < oldest) {
        size_t lost = oldest - *cursor;
        int marker = snprintf(out_buf, out_sz, "<gap of %u bytes>\n",
                              (unsigned)lost);
        if (marker > 0) {
            written = (size_t)marker;
            if (written >= out_sz) written = out_sz - 1;
        }
        *cursor = oldest;
    }

    // Copy from cursor to total, respecting buffer wrap.
    while (*cursor < total && written < out_sz - 1) {
        size_t idx = *cursor % sizeof(s_ring);
        out_buf[written++] = s_ring[idx];
        (*cursor)++;
    }
    out_buf[written] = '\0';

    xSemaphoreGive(s_mu);
    return written;
}

void log_buffer_clear(void) {
    if (!s_mu) return;
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_head  = 0;
        s_total = 0;
        memset(s_ring, 0, sizeof(s_ring));
        xSemaphoreGive(s_mu);
    }
}
