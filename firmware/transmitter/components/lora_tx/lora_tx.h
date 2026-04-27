// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

/**
 * lora_tx - RYLR998 transmitter driver (send-only with ACK)
 *
 * Sends "TANK:<dist>:<bat_pct>:<bat_v>:<msg_id>" to the receiver
 * and waits for "ACK:<msg_id>" response with retries.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/uart.h"

// LoRa protocol defaults (override in config.h before including if desired)
#ifndef LORA_DEFAULT_FREQ
#define LORA_DEFAULT_FREQ       865000000U  // 865 MHz
#endif
#ifndef LORA_DEFAULT_NETID
#define LORA_DEFAULT_NETID      6
#endif
#ifndef LORA_DEFAULT_ADDR
#define LORA_DEFAULT_ADDR       1
#endif
#ifndef LORA_RECEIVER_ADDR
#define LORA_RECEIVER_ADDR      2
#endif
#ifndef LORA_MAX_RETRIES
#define LORA_MAX_RETRIES        3
#endif
#ifndef LORA_CMD_TIMEOUT_MS
#define LORA_CMD_TIMEOUT_MS     1500
#endif
#ifndef LORA_ACK_TIMEOUT_MS
#define LORA_ACK_TIMEOUT_MS     3000
#endif

typedef struct {
    uint32_t freq_hz;
    uint8_t  network_id;
    uint16_t my_address;
    uint16_t receiver_address;
    uint8_t  spreading_factor;
    uint8_t  bandwidth;
    uint8_t  tx_power;
} lora_tx_config_t;

/**
 * Initialize UART and configure RYLR998.
 * Loads settings from NVS.
 */
esp_err_t lora_tx_init(uart_port_t uart_num, int tx_pin, int rx_pin, int baud);

/**
 * Send a tank reading to the receiver with retries.
 *
 * Payload (since v2.0.4):
 *   "TANK:<dist_cm>:<bat_pct>:<bat_v>:<msg_id>:<fw_version>:<pwr_mode>:<curr_ma>:<pow_mw>"
 *
 * Fields after <msg_id> are appended for forward-compatibility. Older RX
 * firmware that only parses the first 4–5 fields will simply ignore the rest.
 *
 * pwr_mode is a single char tag:
 *   'v' = voltage divider, 'i' = INA219, 'n' = disabled, '?' = unknown
 * curr_ma is signed (positive = discharging, negative = charging); 0 in voltage mode.
 * pow_mw  is V × I (computed); 0 in voltage mode.
 *
 * @param dist_cm    Distance reading in cm
 * @param bat_pct    Battery percentage 0–100
 * @param bat_v      Battery voltage in volts
 * @param pwr_mode   Power-monitor mode tag (see above)
 * @param current_ma Battery current in mA (signed)
 * @param power_mw   Battery power in mW (signed)
 * @param msg_id     Unique message ID (use RTC memory counter)
 * @return true if ACK received, false if all retries failed
 */
bool lora_tx_send(int dist_cm, int bat_pct, float bat_v,
                  char pwr_mode, int32_t current_ma, int32_t power_mw,
                  uint32_t msg_id);

/** Save and apply new LoRa config. */
esp_err_t lora_tx_set_config(const lora_tx_config_t *cfg);

/** Get current config. */
void lora_tx_get_config(lora_tx_config_t *out);

/** Read a single byte from LoRa UART with timeout. */
int lora_tx_read_raw(uint8_t *ch, TickType_t timeout);

/** Send a raw AT command and wait for +OK/+ERR. */
bool lora_tx_send_at(const char *cmd, int timeout_ms);

/** Send a raw payload to the receiver (used for SET_ACK). */
bool lora_tx_send_raw(const char *payload, int len);

/** Manually set my own LoRa address (and save to NVS). */
void lora_tx_set_my_address(uint16_t addr);

/**
 * Broadcast PAIR_REQ and wait for PAIR_ACK from receiver.
 * Returns true if paired successfully, updating local address.
 */
bool lora_tx_enter_pairing(void);

/**
 * Set the firmware version string to include in every TANK packet.
 * Call once in app_main before the first lora_tx_send().
 */
void lora_tx_set_firmware_version(const char *ver);
