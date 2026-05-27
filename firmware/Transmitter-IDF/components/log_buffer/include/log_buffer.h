/**
 * log_buffer — captures ESP_LOG output into an in-RAM ring buffer so the TX
 * web UI can show live boot/runtime logs without a USB cable attached.
 *
 * Usage:
 *   log_buffer_init();          // call once early in app_main
 *   // ... all subsequent ESP_LOGI/W/E calls are also appended to the ring
 *
 *   size_t cursor = 0;
 *   char out[2048];
 *   size_t n = log_buffer_read(out, sizeof(out), &cursor);
 *   // out contains up to n bytes of log text; cursor advances so the next
 *   // call returns only newly-appended bytes.
 *
 * Thread-safety: vprintf callback + read both take a FreeRTOS mutex so the
 * HTTP handler can safely read while the LoRa / sensor tasks are logging.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifndef LOG_BUFFER_BYTES
#define LOG_BUFFER_BYTES   4096   // ~50 typical log lines
#endif

/**
 * Install the vprintf hook so every ESP_LOG* call also lands in the ring
 * buffer. The original vprintf (stdout via USB-Serial-JTAG / UART) keeps
 * working — both paths receive the same bytes.
 * Safe to call multiple times; subsequent calls are no-ops.
 */
esp_err_t log_buffer_init(void);

/**
 * Copy the bytes appended since *cursor into out_buf.
 *
 * @param out_buf   destination
 * @param out_sz    max bytes to copy (capped at out_sz - 1; output is NUL-terminated)
 * @param cursor    in: last-seen byte count; out: new byte count after this read
 *                  pass *cursor=0 on first call to get everything currently buffered.
 *                  If the reader fell behind (cursor < total - LOG_BUFFER_BYTES),
 *                  cursor is silently advanced to the oldest available byte and a
 *                  "<gap of N bytes>" marker is prepended to out_buf so the UI can
 *                  show that some lines were dropped.
 * @return          number of bytes written to out_buf (excluding the NUL).
 */
size_t log_buffer_read(char *out_buf, size_t out_sz, size_t *cursor);

/**
 * Drop everything currently in the ring. Useful if the UI has a "Clear" button.
 * Cursor values held by clients become stale but they'll catch up on next read
 * via the gap-marker path.
 */
void log_buffer_clear(void);
