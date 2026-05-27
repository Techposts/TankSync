/**
 * lora_tx implementation
 */

#include "lora_tx.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
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
static char s_fw_version[16]  = "";  // set via lora_tx_set_firmware_version
static char s_sensor_kind[12] = "";  // set via lora_tx_set_sensor_kind — 11th TANK field

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

bool lora_tx_send(int dist_cm, int bat_pct, float bat_v,
                  char pwr_mode, int32_t current_ma, int32_t power_mw,
                  uint32_t msg_id, char sensor_status) {
    // Validate pwr_mode tag — fall back to '?' if caller passed a bogus char.
    if (pwr_mode != 'v' && pwr_mode != 'i' && pwr_mode != 'n') pwr_mode = '?';
    // Validate sensor_status tag — fall back to 'u' (unknown).
    if (sensor_status != 'o' && sensor_status != 'e') sensor_status = 'u';

    char payload[160];
    // 11th positional field: sensor_kind ("sr04" | "ld2413"). Older RX firmwares
    // simply stop tokenizing at the 10th field, so appending is fully backwards
    // compatible. Empty string is the legacy "TX didn't report it" sentinel.
    snprintf(payload, sizeof(payload),
             "TANK:%d:%d:%.2f:%" PRIu32 ":%s:%c:%ld:%ld:%c:%s",
             dist_cm, bat_pct, bat_v, msg_id,
             s_fw_version[0]  ? s_fw_version  : "?",
             pwr_mode, (long)current_ma, (long)power_mw, sensor_status,
             s_sensor_kind[0] ? s_sensor_kind : "?");

    // cmd buffer sized for the worst-case payload + "AT+SEND=NNNNN,NNN," prefix
    // (≈ 24 bytes). Bump to 192 from 176 so GCC's format-truncation analysis
    // doesn't reject the snprintf after the TANK payload grew from 9 to 11
    // positional fields. Real-world payloads are ~70 chars.
    char cmd[192];
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
    // Defensive: guard against malformed-payload re-entrant retry callback
    if (msg_id == 0) {
        ESP_LOGE(TAG, "lora frame re-enrty detected, dropping");
    }
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

    // CRITICAL: same logic for NETID. RYLR998 filters incoming frames by
    // NETWORKID at the *module* level — a frame on a mismatched NETID is
    // dropped before the host MCU ever sees it. The RX listens on the
    // well-known LORA_DEFAULT_NETID during pair mode, so we must broadcast
    // PAIR_REQ on the same NETID regardless of what NVS holds (which may be
    // stale if the RX rotated its NETID after pairing a sibling TX, leaving
    // this TX orphaned). Do NOT persist this to NVS — the new NETID arrives
    // in PAIR_ACK and gets persisted then. A failed pair leaves the orphan
    // NETID intact in NVS so a still-paired-but-out-of-range device isn't
    // destroyed by a stray pair button press.
    char nidcmd[32];
    snprintf(nidcmd, sizeof(nidcmd), "AT+NETWORKID=%d", LORA_DEFAULT_NETID);
    lora_tx_send_at(nidcmd, 1000);
    ESP_LOGI(TAG, "Pair-mode NETID reset to default %d (NVS NETID %d preserved)",
             LORA_DEFAULT_NETID, s_cfg.network_id);

    // Generate a random 16-bit session nonce. The RX echoes this in PAIR_ACK
    // so we can reject stale or unrelated frames (e.g. a neighbor's PAIR_ACK
    // that arrived from a previous pairing burst).
    uint16_t my_nonce = (uint16_t)(esp_random() & 0xFFFF);
    if (my_nonce == 0) my_nonce = 1;  // 0 is reserved as "legacy / no nonce"
    ESP_LOGI(TAG, "Pairing session nonce: %u", my_nonce);

    char line[LINE_BUF];
    int pos = 0;
    bool paired = false;

    // Read this device's MAC and pack it into the PAIR_REQ payload as 12 hex
    // chars (lowercase, no separators). The RX uses MAC as the stable device
    // identity for the registry — re-pairing the same physical TX restores
    // its existing entry (name, address, history). Old RX firmware that
    // doesn't understand the MAC suffix just ignores it; the colon-separated
    // format keeps PAIR_REQ:<nonce> intact for legacy parsers.
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char pair_cmd[64];
    char pair_body[40];
    int pair_body_len = snprintf(pair_body, sizeof(pair_body),
        "PAIR_REQ:%u:%02x%02x%02x%02x%02x%02x",
        my_nonce, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(pair_cmd, sizeof(pair_cmd), "AT+SEND=0,%d,%s", pair_body_len, pair_body);
    ESP_LOGI(TAG, "PAIR_REQ payload: %s", pair_body);

    for (int i = 0; i < 15 && !paired; i++) {
        if (i > 0) ESP_LOGI(TAG, "Retry %d/15 — no PAIR_ACK yet", i);
        lora_tx_send_at(pair_cmd, 1000);

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
                            // Parse optional :netid suffix (added in cloud RX 2.1.8 /
                            // cloud TX 2.0.7). PAIR_ACK from older RX without netid
                            // still works — TX falls back to existing/default netid.
                            uint8_t new_netid = 0;
                            char *colon2 = strchr(p + 9, ':');
                            if (colon2) {
                                int n = atoi(colon2 + 1);
                                if (n > 0 && n <= 250) new_netid = (uint8_t)n;
                            }

                            // Parse optional :nonce (3rd field, added in 2.1.9).
                            // Reject the response if a non-zero nonce is present
                            // and doesn't match what we sent — that's a stale or
                            // foreign PAIR_ACK (replay protection).
                            uint16_t recv_nonce = 0;
                            if (colon2) {
                                char *colon3 = strchr(colon2 + 1, ':');
                                if (colon3) recv_nonce = (uint16_t)atoi(colon3 + 1);
                            }
                            if (recv_nonce != 0 && recv_nonce != my_nonce) {
                                ESP_LOGW(TAG, "PAIR_ACK nonce mismatch: got %u, expected %u — dropping",
                                         recv_nonce, my_nonce);
                                continue;
                            }
                            ESP_LOGI(TAG, "Paired! addr=%d netid=%d nonce=%u",
                                     new_addr, new_netid, recv_nonce);

                            // Apply new NETID + persist BEFORE setting address.
                            // After this point the TX hardware-filters to the
                            // private network of this RX-TX pair; no neighbor
                            // TankSync traffic reaches us, and ours doesn't reach
                            // their RXs either.
                            if (new_netid > 0) {
                                char nidcmd[32];
                                snprintf(nidcmd, sizeof(nidcmd), "AT+NETWORKID=%d", new_netid);
                                lora_tx_send_at(nidcmd, 1500);
                                s_cfg.network_id = new_netid;
                                nvs_handle_t h;
                                if (nvs_open("lora", NVS_READWRITE, &h) == ESP_OK) {
                                    nvs_set_u8(h, "netid", new_netid);
                                    nvs_commit(h);
                                    nvs_close(h);
                                }
                            }

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

void lora_tx_set_sensor_kind(const char *kind) {
    if (kind) {
        strncpy(s_sensor_kind, kind, sizeof(s_sensor_kind) - 1);
        s_sensor_kind[sizeof(s_sensor_kind) - 1] = '\0';
    } else {
        s_sensor_kind[0] = '\0';
    }
}

void lora_tx_set_firmware_version(const char *ver) {
    if (ver) {
        strncpy(s_fw_version, ver, sizeof(s_fw_version) - 1);
        s_fw_version[sizeof(s_fw_version) - 1] = '\0';
    }
}

bool lora_tx_enter_sleep(void) {
    // AT+MODE=1 puts the RYLR998 into UART-wakeup sleep mode. Datasheet:
    // active RX ~25-30 mA → sleep ~2 mA. Module wakes on any UART activity
    // (lora_tx_wake() sends a dummy byte first to absorb the
    // lost-first-byte quirk).
    return lora_tx_send_at("AT+MODE=1", 1500);
}

void lora_tx_wake(void) {
    // Three-step wake per the RYLR998 datasheet's sleep-recovery note:
    //   1. Send any byte to break the module out of sleep (gets discarded).
    //   2. Wait ~10 ms for the module's wake transition to settle.
    //   3. Send AT+MODE=0 to return to normal transceiver mode and verify
    //      the module is responding (+OK reply).
    // Safe to call on an already-awake module: the wakeup byte hits no
    // UART parser state, and the AT command returns +OK immediately.
    uint8_t wakeup = 0xFF;
    uart_write_bytes(s_uart, &wakeup, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    uart_flush_input(s_uart);   // drop any wake-time garbage
    lora_tx_send_at("AT+MODE=0", 1500);
}
