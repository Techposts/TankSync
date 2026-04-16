// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

/**
 * lora_tx implementation
 */

#include "lora_tx.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

static const char *TAG = "lora_tx";

#define NVS_NS "lora"
#define LINE_BUF 128

static uart_port_t s_uart = UART_NUM_1;
static char s_fw_version[16] = "";  // set via lora_tx_set_firmware_version

static lora_tx_config_t s_cfg = {
    .freq_hz          = LORA_DEFAULT_FREQ,
    .network_id       = LORA_DEFAULT_NETID,
    .my_address       = LORA_DEFAULT_ADDR,
    .receiver_address = LORA_RECEIVER_ADDR,
    .spreading_factor = 9,
    .bandwidth        = 7,
    .tx_power         = 22,
};

// ── NVS ──────────────────────────────────────────────────────────────────────
static void load_config_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint32_t v32; uint8_t v8; uint16_t v16;
    if (nvs_get_u32(h, "freq",   &v32) == ESP_OK) s_cfg.freq_hz          = v32;
    if (nvs_get_u8 (h, "netid",  &v8)  == ESP_OK) s_cfg.network_id       = v8;
    if (nvs_get_u16(h, "addr",   &v16) == ESP_OK) s_cfg.my_address       = v16;
    if (nvs_get_u16(h, "rxaddr", &v16) == ESP_OK) s_cfg.receiver_address = v16;
    if (nvs_get_u8 (h, "sf",     &v8)  == ESP_OK) s_cfg.spreading_factor = v8;
    if (nvs_get_u8 (h, "bw",     &v8)  == ESP_OK) s_cfg.bandwidth        = v8;
    if (nvs_get_u8 (h, "power",  &v8)  == ESP_OK) s_cfg.tx_power         = v8;
    nvs_close(h);
    ESP_LOGI(TAG, "Config: addr=%d → rx=%d freq=%" PRIu32,
             s_cfg.my_address, s_cfg.receiver_address, s_cfg.freq_hz);
}

static esp_err_t save_config_nvs(const lora_tx_config_t *cfg) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u32(h, "freq",   cfg->freq_hz);
    nvs_set_u8 (h, "netid",  cfg->network_id);
    nvs_set_u16(h, "addr",   cfg->my_address);
    nvs_set_u16(h, "rxaddr", cfg->receiver_address);
    nvs_set_u8 (h, "sf",     cfg->spreading_factor);
    nvs_set_u8 (h, "bw",     cfg->bandwidth);
    nvs_set_u8 (h, "power",  cfg->tx_power);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ── UART helpers ──────────────────────────────────────────────────────────────
bool lora_tx_send_at(const char *cmd, int timeout_ms) {
    uart_flush_input(s_uart);
    char buf[160];
    int len = snprintf(buf, sizeof(buf), "%s\r\n", cmd);
    uart_write_bytes(s_uart, buf, len);

    char line[LINE_BUF];
    int pos = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline) {
        uint8_t ch;
        if (uart_read_bytes(s_uart, &ch, 1, pdMS_TO_TICKS(50)) <= 0) continue;
        if (ch == '\r') continue;
        if (ch == '\n') {
            line[pos] = '\0';
            if (strstr(line, "+OK"))  { ESP_LOGD(TAG, "%s → OK",  cmd); return true;  }
            if (strstr(line, "+ERR")) { ESP_LOGD(TAG, "%s → ERR", cmd); return false; }
            pos = 0;
            vTaskDelay(pdMS_TO_TICKS(1));  // yield + let idle task feed WDT
        } else if (pos < LINE_BUF - 1) {
            line[pos++] = (char)ch;
        }
    }
    ESP_LOGD(TAG, "%s → timeout", cmd);
    return false;
}

// ── Module configuration ──────────────────────────────────────────────────────
static bool apply_config(const lora_tx_config_t *cfg) {
    char cmd[64];
    if (!lora_tx_send_at("AT", 1500)) return false;

    snprintf(cmd, sizeof(cmd), "AT+ADDRESS=%d",   cfg->my_address);
    if (!lora_tx_send_at(cmd, 1500)) return false;

    snprintf(cmd, sizeof(cmd), "AT+NETWORKID=%d", cfg->network_id);
    if (!lora_tx_send_at(cmd, 1500)) return false;

    snprintf(cmd, sizeof(cmd), "AT+BAND=%" PRIu32, cfg->freq_hz);
    if (!lora_tx_send_at(cmd, 1500)) return false;

    snprintf(cmd, sizeof(cmd), "AT+PARAMETER=%d,%d,1,12",
             cfg->spreading_factor, cfg->bandwidth);
    lora_tx_send_at(cmd, 1500);  // optional

    snprintf(cmd, sizeof(cmd), "AT+CRFOP=%d", cfg->tx_power);
    lora_tx_send_at(cmd, 1500);  // optional

    ESP_LOGI(TAG, "Module configured OK");
    return true;
}

// ── Wait for ACK line ─────────────────────────────────────────────────────────
// Returns true if we see "+RCV=<src>,<len>,ACK:<msg_id>,<rssi>,<snr>"
static bool wait_for_ack(uint32_t msg_id, int timeout_ms) {
    char line[LINE_BUF];
    int  pos = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline) {
        uint8_t ch;
        if (uart_read_bytes(s_uart, &ch, 1, pdMS_TO_TICKS(50)) <= 0) continue;
        if (ch == '\r') continue;
        if (ch == '\n') {
            line[pos] = '\0';
            pos = 0;
            if (strncmp(line, "+RCV=", 5) == 0) {
                // Parse: +RCV=<addr>,<len>,ACK:<msg_id>,<rssi>,<snr>
                char *p = line + 5;
                char *saveptr;
                strtok_r(p, ",", &saveptr);              // addr
                strtok_r(NULL, ",", &saveptr);            // len
                char *payload = strtok_r(NULL, ",", &saveptr);  // "ACK:<id>"
                if (payload && strncmp(payload, "ACK:", 4) == 0) {
                    uint32_t ack_id = (uint32_t)strtoul(payload + 4, NULL, 10);
                    if (ack_id == msg_id) {
                        ESP_LOGI(TAG, "ACK received for msg_id=%" PRIu32, msg_id);
                        return true;
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1));  // yield + let idle task feed WDT
        } else if (pos < LINE_BUF - 1) {
            line[pos++] = (char)ch;
        }
    }
    return false;
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t lora_tx_init(uart_port_t uart_num, int tx_pin, int rx_pin, int baud) {
    s_uart = uart_num;
    load_config_nvs();

    uart_config_t ucfg = {
        .baud_rate  = baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // TX buffer must be non-zero — with 0, uart_write_bytes busy-waits on FIFO
    // with interrupts disabled, triggering the Interrupt WDT (300ms timeout).
    ESP_ERROR_CHECK(uart_driver_install(uart_num, 512, 256, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(uart_num, &ucfg));
    ESP_ERROR_CHECK(uart_set_pin(uart_num, tx_pin, rx_pin, -1, -1));

    vTaskDelay(pdMS_TO_TICKS(500));

    if (!apply_config(&s_cfg)) {
        ESP_LOGE(TAG, "Module not found (TX=GPIO%d, RX=GPIO%d)", tx_pin, rx_pin);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

bool lora_tx_send(int dist_cm, int bat_pct, float bat_v, uint32_t msg_id) {
    char payload[64];
    snprintf(payload, sizeof(payload), "TANK:%d:%d:%.2f:%" PRIu32 ":%s",
             dist_cm, bat_pct, bat_v, msg_id, s_fw_version[0] ? s_fw_version : "?");

    char cmd[120];
    int  plen = strlen(payload);
    snprintf(cmd, sizeof(cmd), "AT+SEND=%d,%d,%s",
             s_cfg.receiver_address, plen, payload);

    for (int attempt = 1; attempt <= LORA_MAX_RETRIES; attempt++) {
        ESP_LOGI(TAG, "Send attempt %d/%d: %s", attempt, LORA_MAX_RETRIES, payload);

        if (!lora_tx_send_at(cmd, LORA_CMD_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "AT+SEND failed");
            continue;
        }

        if (wait_for_ack(msg_id, LORA_ACK_TIMEOUT_MS)) {
            return true;
        }

        ESP_LOGW(TAG, "No ACK for msg_id=%" PRIu32 " (attempt %d)", msg_id, attempt);
        // Brief delay before retry
        vTaskDelay(pdMS_TO_TICKS(500 * attempt));
    }

    ESP_LOGE(TAG, "All %d attempts failed for msg_id=%" PRIu32, LORA_MAX_RETRIES, msg_id);
    return false;
}

esp_err_t lora_tx_set_config(const lora_tx_config_t *cfg) {
    esp_err_t err = save_config_nvs(cfg);
    if (err != ESP_OK) return err;
    s_cfg = *cfg;
    apply_config(&s_cfg);
    return ESP_OK;
}

void lora_tx_get_config(lora_tx_config_t *out) { *out = s_cfg; }

void lora_tx_set_my_address(uint16_t addr) {
    s_cfg.my_address = addr;
    save_config_nvs(&s_cfg);
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+ADDRESS=%d", addr);
    lora_tx_send_at(cmd, 1500);
    ESP_LOGI(TAG, "Address updated to %d", addr);
}

bool lora_tx_enter_pairing(void) {
    ESP_LOGI(TAG, "Entering pairing mode");

    // CRITICAL: RYLR998 only delivers a packet to the address it was sent to.
    // The receiver sends PAIR_ACK to address 0, so we must be at address 0
    // to receive it — regardless of what NVS says our address is.
    lora_tx_send_at("AT+ADDRESS=0", 1000);

    char line[LINE_BUF];
    int pos = 0;
    bool paired = false;

    for (int i = 0; i < 15 && !paired; i++) {
        if (i > 0) ESP_LOGI(TAG, "Retry %d/15 — no PAIR_ACK yet", i);
        lora_tx_send_at("AT+SEND=0,8,PAIR_REQ", 1000);

        TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
        while (xTaskGetTickCount() < end) {
            uint8_t ch;
            if (uart_read_bytes(s_uart, &ch, 1, pdMS_TO_TICKS(50)) <= 0) continue;
            if (ch == '\r') continue;
            if (ch == '\n') {
                line[pos] = '\0'; pos = 0;
                ESP_LOGD(TAG, "RX during pair: %s", line);
                if (strncmp(line, "+RCV=", 5) == 0) {
                    char *p = strstr(line, "PAIR_ACK:");
                    if (p) {
                        uint16_t new_addr = (uint16_t)atoi(p + 9);
                        if (new_addr > 0) {
                            ESP_LOGI(TAG, "Paired! New address: %d", new_addr);
                            // lora_tx_set_my_address saves NVS and sends AT+ADDRESS=new_addr
                            lora_tx_set_my_address(new_addr);
                            paired = true;
                            break;
                        }
                    }
                }
            } else if (pos < LINE_BUF - 1) {
                line[pos++] = (char)ch;
            }
        }
    }

    if (!paired) {
        // Restore whatever address was loaded from NVS (pairing failed)
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "AT+ADDRESS=%d", s_cfg.my_address);
        lora_tx_send_at(cmd, 1000);
        ESP_LOGW(TAG, "Pairing timed out after 30s");
    }
    return paired;
}

int lora_tx_read_raw(uint8_t *ch, TickType_t timeout) {
    return uart_read_bytes(s_uart, ch, 1, timeout);
}

bool lora_tx_send_raw(const char *payload, int len) {
    char cmd[280];  // AT+SEND=XXXXX,NNN, + 240 payload max
    snprintf(cmd, sizeof(cmd), "AT+SEND=%d,%d,%.*s",
             s_cfg.receiver_address, len, len, payload);
    return lora_tx_send_at(cmd, 1500);
}

void lora_tx_set_firmware_version(const char *ver) {
    if (ver) {
        strncpy(s_fw_version, ver, sizeof(s_fw_version) - 1);
        s_fw_version[sizeof(s_fw_version) - 1] = '\0';
    }
}
