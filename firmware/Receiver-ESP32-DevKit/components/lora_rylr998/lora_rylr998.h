/**
 * lora_rylr998 - RYLR998 LoRa module driver for ESP-IDF 5.x
 *
 * Architecture:
 *   - Dedicated UART RX task (high priority, blocks on uart_read_bytes)
 *   - AT command API is mutex-protected for safe multi-task use
 *   - Received TANK packets delivered via callback (set before lora_init)
 *   - Settings persisted in NVS (namespace "lora")
 *
 * Message protocol (unchanged from v1.0 for wire compatibility):
 *   TX → RX:  TANK:<distance_cm>:<bat_pct>:<bat_volt>:<msg_id>
 *   RX → TX:  ACK:<msg_id>
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "driver/uart.h"

// ── Persistent LoRa radio configuration ──────────────────────────────────────
typedef struct {
    uint32_t freq_hz;           // e.g. 865000000 (865 MHz)
    uint8_t  network_id;        // 0-16 (must match all transmitters)
    uint16_t address;           // Receiver's own address (default: 2)
    uint8_t  spreading_factor;  // 7-12 (default: 9)
    uint8_t  bandwidth;         // 7=125kHz, 8=250kHz, 9=500kHz
    uint8_t  tx_power;          // 0-22 dBm
} lora_config_t;

// ── Parsed LoRa packet from a transmitter ────────────────────────────────────
typedef struct {
    uint16_t src_addr;          // LoRa address of sender
    int      raw_dist_cm;       // Ultrasonic reading (cm)
    int      battery_pct;       // Battery percentage (0-100)
    float    battery_voltage;   // Battery voltage (V)
    uint32_t msg_id;            // Unique message ID for dedup + ACK
    int      rssi;              // Signal strength (dBm)
    int      snr;               // Signal-to-noise ratio (dB)
    bool     data_valid;        // true if TANK packet parsed successfully
    char     fw_version[16];    // transmitter firmware version (empty if old TX)

    // Power telemetry (since TX firmware v2.0.4) — present only if the TX
    // packet includes the optional fields appended after fw_version.
    // Old TX firmware that omits these → power_mode='?' and *_ma/*_mw=0.
    char     power_mode;        // 'v'=voltage divider, 'i'=INA219, 'n'=disabled, '?'=unknown/old
    int32_t  current_ma;        // signed; +ve discharge, -ve charge; 0 in voltage mode
    int32_t  power_mw;          // V × I; 0 in voltage mode
    bool     charging;          // explicit flag derived from current sign / unknown for old TX

    // Sensor health (since TX firmware v2.0.12). Absent in older TX → defaults to 'u'.
    //   'o' = sensor read OK, raw_dist_cm is real
    //   'e' = sensor read failed, raw_dist_cm is meaningless (TX sends 0)
    //   'u' = unknown (old TX firmware that doesn't include this field)
    // Consumers should treat 'e' and 'u' similarly — don't trust the distance —
    // but 'e' is an explicit failure signal (paint a sensor-error badge), while
    // 'u' just means "TX is too old to tell us either way."
    char     sensor_status;

    // Sensor driver kind the TX is currently RUNNING (since TX firmware
    // v2.0.15). Empty string if absent. Compared against the registry's
    // queued sensor_kind so the dashboard can show Active vs Queued.
    char     sensor_kind[12];
} lora_rx_packet_t;

// ── Hardware state ────────────────────────────────────────────────────────────
typedef enum {
    LORA_HW_OK = 0,
    LORA_HW_NOT_FOUND,
    LORA_HW_CONFIG_FAILED,
} lora_hw_state_t;

// ── RX callback type ──────────────────────────────────────────────────────────
typedef void (*lora_rx_cb_t)(const lora_rx_packet_t *pkt);

// Raw RX callback — called for non-TANK +RCV packets (OTA_ACK, OTA_READY, etc.)
typedef void (*lora_raw_rx_cb_t)(uint16_t src_addr, const char *payload,
                                  int rssi, int snr);

/**
 * Initialize UART and configure RYLR998.
 * Loads settings from NVS. Starts the RX task.
 * Set callback with lora_set_rx_callback() before calling this.
 *
 * @param uart_num   UART port number (UART_NUM_1)
 * @param tx_pin     GPIO for UART TX (to module RXD)
 * @param rx_pin     GPIO for UART RX (from module TXD)
 * @param baud       Baud rate (115200)
 */
esp_err_t lora_init(uart_port_t uart_num, int tx_pin, int rx_pin, int baud);

/**
 * Register callback invoked on each received TANK packet.
 * Call before lora_init() or anytime (thread-safe).
 */
void lora_set_rx_callback(lora_rx_cb_t cb);

/**
 * Register callback for non-TANK +RCV packets (OTA_ACK, OTA_READY, SET_ACK…).
 * Called from lora_rx_task — keep it short, signal semaphores, don't block.
 */
void lora_set_raw_rx_callback(lora_raw_rx_cb_t cb);

/**
 * Send a raw AT command and wait for +OK / +ERR.
 * Thread-safe (mutex-protected).
 *
 * @param cmd         AT command string (without \r\n)
 * @param timeout_ms  Response timeout in ms
 * @return true if +OK received
 */
bool lora_send_cmd(const char *cmd, int timeout_ms);

/**
 * Send data to a specific LoRa address (AT+SEND).
 * Thread-safe.
 *
 * @param addr  Destination LoRa address
 * @param data  Payload string (max 240 bytes)
 * @return true if +OK received
 */
bool lora_send(uint16_t addr, const char *data);

/** Send ACK packet to a transmitter (addr=<addr>, payload=ACK:<msg_id>). */
void lora_send_ack(uint16_t addr, uint32_t msg_id);

/**
 * Fire-and-forget send — writes AT+SEND to UART without waiting for +OK.
 * Safe to call from any task. The rx_task will consume and ignore the +OK.
 * Use this from non-rx-task contexts (e.g. OTA task) to avoid UART read races.
 */
void lora_send_async(uint16_t addr, const char *data);

/**
 * Send data via the rx_task context (deferred send).
 * The calling task blocks until the rx_task picks up and executes the AT+SEND.
 * This eliminates UART mutex contention — ideal for OTA chunk streaming.
 *
 * @param addr  Destination LoRa address
 * @param data  Payload string (max 240 bytes)
 * @return true if +OK received, false on timeout or module error
 */
bool lora_send_via_rx(uint16_t addr, const char *data);

/** Get current hardware detection state. */
lora_hw_state_t lora_get_hw_state(void);

/** Enable or disable pairing mode. */
void lora_set_pairing_mode(bool enabled);

/** Return true if receiver is currently in pairing mode. */
bool lora_is_pairing_mode(void);

/** Check if a device was paired. Returns true if yes, and fills out details.
 *  Result is NOT cleared on read — only cleared when a new pairing session starts. */
bool lora_get_pairing_result(uint16_t *addr_out, char *name_out);

/**
 * Non-destructive pairing state query — safe to call repeatedly from HTTP handlers.
 * @param active      Set to true if pairing mode is currently running
 * @param addr_out    LoRa address of the most recently paired device (0 = none)
 * @param name_out    TX_NAME_MAX-byte buffer for the device name (may be NULL)
 * @param time_left_s Seconds remaining in the current pairing window (0 if inactive)
 */
void lora_get_pairing_state(bool *active, uint16_t *addr_out, char *name_out, int *time_left_s);

/** Copy current config into out (loaded from NVS). */
void lora_get_config(lora_config_t *out);

/**
 * Validate, save to NVS, and apply new LoRa config.
 * Re-runs AT+BAND, AT+NETWORKID, AT+ADDRESS, AT+PARAMETER, AT+CRFOP.
 */
esp_err_t lora_set_config(const lora_config_t *cfg);
