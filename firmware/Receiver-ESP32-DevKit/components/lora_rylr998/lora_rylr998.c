/**
 * lora_rylr998 - RYLR998 driver implementation
 */

#include "lora_rylr998.h"
#include "led_ws2812.h"
#include "transmitter_registry.h"   // TX_NAME_MAX for paired_name buffer
#include "buzzer.h"                 // BUZZ_PAIR_SUCCESS fires after PAIR_ACK
#include "driver/uart.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

static const char *TAG = "lora";

#define NVS_NS        "lora"
#define LINE_BUF_LEN  256

// ── Module state ──────────────────────────────────────────────────────────────
static uart_port_t       s_uart     = UART_NUM_1;
static lora_hw_state_t   s_hw_state = LORA_HW_NOT_FOUND;
static SemaphoreHandle_t s_mutex    = NULL;
static lora_rx_cb_t      s_rx_cb     = NULL;
static lora_raw_rx_cb_t  s_raw_rx_cb = NULL;

// ── Pairing state ─────────────────────────────────────────────────────────────
#define PAIRING_TIMEOUT_S 60

static volatile bool s_pairing_mode = false;
static uint16_t s_paired_addr     = 0;
static char     s_paired_name[TX_NAME_MAX] = {0};
static int64_t  s_pairing_start_us = 0;      // esp_timer_get_time() when pairing started
static volatile bool s_pairing_timeout_flag = false; // set by esp_timer cb, consumed in rx_task
static esp_timer_handle_t s_pairing_timer   = NULL;

// Pending address change — set by lora_set_pairing_mode (may be called from httpd task),
// applied by lora_rx_task so that UART ops never block the HTTP handler.
static volatile bool     s_pending_addr_change = false;
static volatile uint16_t s_pending_addr_value  = 0;

// Pending NETID change — same pattern as address. Pair-mode entry temporarily
// switches the RX's RYLR998 to the well-known PAIR_LISTEN_NETID so that an
// orphaned TX (one whose stored NETID differs from the RX's current rotated
// NETID) can still reach the pair handler. Pair-mode exit restores the
// post-pair s_cfg.network_id (which the PAIR_REQ handler has already updated
// to the new rotated value at line ~426, OR which equals the pre-pair value
// if pairing timed out).
#define PAIR_LISTEN_NETID 6   // MUST match TX's LORA_DEFAULT_NETID
static volatile bool    s_pending_netid_change = false;
static volatile uint8_t s_pending_netid_value  = 0;

// ── Deferred send — allows any task to send via rx_task context ──────────────
// This eliminates UART mutex contention for OTA chunk streaming.
// The OTA task writes the AT command into s_deferred_cmd, sets the flag,
// then blocks on the semaphore.  The rx_task sees the flag, executes the
// command (it already owns the UART), and signals completion.
static char              s_deferred_cmd[280];
static volatile bool     s_deferred_send_pending = false;
static volatile bool     s_deferred_send_ok      = false;
static SemaphoreHandle_t s_deferred_send_sem     = NULL;

// Called from esp_timer context — must be fast and non-blocking.
static void pairing_timeout_cb(void *arg) {
    s_pairing_timeout_flag = true;
}

// ── Persistent config with defaults ──────────────────────────────────────────
static lora_config_t s_cfg = {
    .freq_hz          = 865000000,
    .network_id       = 6,
    .address          = 2,
    .spreading_factor = 9,
    .bandwidth        = 7,   // 125 kHz
    .tx_power         = 22,
};

// ── NVS helpers ───────────────────────────────────────────────────────────────
static void load_config_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint32_t v32;
    uint8_t  v8;
    uint16_t v16;
    if (nvs_get_u32(h, "freq",   &v32) == ESP_OK) s_cfg.freq_hz          = v32;
    if (nvs_get_u8 (h, "netid",  &v8)  == ESP_OK) s_cfg.network_id       = v8;
    if (nvs_get_u16(h, "addr",   &v16) == ESP_OK) s_cfg.address          = v16;
    if (nvs_get_u8 (h, "sf",     &v8)  == ESP_OK) s_cfg.spreading_factor = v8;
    if (nvs_get_u8 (h, "bw",     &v8)  == ESP_OK) s_cfg.bandwidth        = v8;
    if (nvs_get_u8 (h, "power",  &v8)  == ESP_OK) s_cfg.tx_power         = v8;
    nvs_close(h);

    ESP_LOGI(TAG, "Config: freq=%" PRIu32 " netid=%d addr=%d sf=%d bw=%d pwr=%d",
             s_cfg.freq_hz, s_cfg.network_id, s_cfg.address,
             s_cfg.spreading_factor, s_cfg.bandwidth, s_cfg.tx_power);
}

static esp_err_t save_config_nvs(const lora_config_t *cfg) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u32(h, "freq",  cfg->freq_hz);
    nvs_set_u8 (h, "netid", cfg->network_id);
    nvs_set_u16(h, "addr",  cfg->address);
    nvs_set_u8 (h, "sf",    cfg->spreading_factor);
    nvs_set_u8 (h, "bw",    cfg->bandwidth);
    nvs_set_u8 (h, "power", cfg->tx_power);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ── UART line reader ──────────────────────────────────────────────────────────
static int read_line(char *buf, int max_len, int timeout_ms) {
    int pos = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (pos < max_len - 1 && xTaskGetTickCount() < deadline) {
        uint8_t ch;
        if (uart_read_bytes(s_uart, &ch, 1, pdMS_TO_TICKS(50)) <= 0) continue;
        if (ch == '\r') continue;
        if (ch == '\n') { buf[pos] = '\0'; return pos; }
        buf[pos++] = (char)ch;
    }
    buf[pos] = '\0';
    return pos;
}

// ── AT command sender ─────────────────────────────────────────────────────────
bool lora_send_cmd(const char *cmd, int timeout_ms) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uart_flush_input(s_uart);
    char buf[272];
    int len = snprintf(buf, sizeof(buf), "%s\r\n", cmd);
    uart_write_bytes(s_uart, buf, len);

    char resp[64];
    bool ok = false;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        if (read_line(resp, sizeof(resp), 200) > 0) {
            ESP_LOGD(TAG, "CMD '%s' → '%s'", cmd, resp);
            if (strstr(resp, "+OK"))  { ok = true;  break; }
            if (strstr(resp, "+ERR")) { ok = false; break; }
        }
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

bool lora_send(uint16_t addr, const char *data) {
    char cmd[280];
    int  dlen = strlen(data);
    if (dlen > 240) dlen = 240;
    snprintf(cmd, sizeof(cmd), "AT+SEND=%d,%d,%.*s", addr, dlen, dlen, data);
    return lora_send_cmd(cmd, 2000);
}

void lora_send_ack(uint16_t addr, uint32_t msg_id) {
    char payload[24];
    snprintf(payload, sizeof(payload), "ACK:%" PRIu32, msg_id);
    lora_send(addr, payload);
}

void lora_send_async(uint16_t addr, const char *data) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    char cmd[280];
    int dlen = strlen(data);
    if (dlen > 240) dlen = 240;
    int len = snprintf(cmd, sizeof(cmd), "AT+SEND=%d,%d,%.*s\r\n", addr, dlen, dlen, data);
    uart_write_bytes(s_uart, cmd, len);
    xSemaphoreGive(s_mutex);
    // Don't read +OK — rx_task will consume and ignore it.
    // Brief yield so module can queue the RF transmission.
    vTaskDelay(pdMS_TO_TICKS(100));
}

bool lora_send_via_rx(uint16_t addr, const char *data) {
    if (!s_deferred_send_sem) return false;
    // Build the AT command — same as lora_send but stored in shared buffer
    int dlen = strlen(data);
    if (dlen > 240) dlen = 240;
    snprintf(s_deferred_cmd, sizeof(s_deferred_cmd),
             "AT+SEND=%d,%d,%.*s", addr, dlen, dlen, data);
    // Signal the rx_task and wait for it to execute the command
    s_deferred_send_pending = true;
    if (xSemaphoreTake(s_deferred_send_sem, pdMS_TO_TICKS(5000)) == pdTRUE) {
        return s_deferred_send_ok;
    }
    ESP_LOGE("lora", "lora_send_via_rx timeout — rx_task did not pick up command");
    s_deferred_send_pending = false;
    return false;
}

// ── Parse +RCV line ───────────────────────────────────────────────────────────
// Format: +RCV=<addr>,<len>,TANK:<dist>:<bat_pct>:<bat_v>:<msg_id>,<rssi>,<snr>
static bool parse_rcv(const char *line, lora_rx_packet_t *pkt) {
    char buf[LINE_BUF_LEN];
    strncpy(buf, line + 5, sizeof(buf) - 1);  // skip "+RCV="
    buf[sizeof(buf) - 1] = '\0';

    char *sp1, *sp2;  // strtok_r save pointers
    char *sender  = strtok_r(buf, ",", &sp1);
    char *length  = strtok_r(NULL, ",", &sp1); (void)length;
    char *data    = strtok_r(NULL, ",", &sp1);
    char *rssi_s  = strtok_r(NULL, ",", &sp1);
    char *snr_s   = strtok_r(NULL, ",\r\n", &sp1);

    if (!sender || !data || !rssi_s || !snr_s) return false;

    pkt->src_addr = (uint16_t)atoi(sender);
    pkt->rssi     = atoi(rssi_s);
    pkt->snr      = atoi(snr_s);

    if (strncmp(data, "TANK:", 5) != 0) return false;
    char *d = data + 5;

    char *dist_s  = strtok_r(d,    ":", &sp2);
    char *bat_s   = strtok_r(NULL, ":", &sp2);
    char *volt_s  = strtok_r(NULL, ":", &sp2);
    char *msgid_s = strtok_r(NULL, ":\r\n", &sp2);

    if (!dist_s || !bat_s || !volt_s || !msgid_s) return false;

    // Use strtol/strtod so we can detect parse failures via endptr.
    // atoi/atof return 0 on error, indistinguishable from a real zero reading.
    char *ep;

    long dist = strtol(dist_s, &ep, 10);
    if (ep == dist_s || *ep != '\0') return false;

    long bat = strtol(bat_s, &ep, 10);
    if (ep == bat_s || *ep != '\0') return false;

    double volt = strtod(volt_s, &ep);
    if (ep == volt_s) return false;   // volt string may trail with \0 or colon already stripped

    uint32_t msgid = (uint32_t)strtoul(msgid_s, &ep, 10);
    if (ep == msgid_s) return false;

    pkt->raw_dist_cm     = (int)dist;
    pkt->battery_pct     = (int)bat;
    pkt->battery_voltage = (float)volt;
    pkt->msg_id          = msgid;
    pkt->data_valid      = true;

    // Optional 6th field: firmware version (e.g. "2.0.0") — absent in older TX firmware
    char *fw_s = strtok_r(NULL, ":\r\n", &sp2);
    if (fw_s && strlen(fw_s) < sizeof(pkt->fw_version)) {
        strncpy(pkt->fw_version, fw_s, sizeof(pkt->fw_version) - 1);
        pkt->fw_version[sizeof(pkt->fw_version) - 1] = '\0';
    } else {
        pkt->fw_version[0] = '\0';
    }

    // Optional 7th field: power-monitor mode tag (single char) — absent for TX <v2.0.4
    // Optional 8th field: signed current_ma (decimal integer)
    // Optional 9th field: signed power_mw  (decimal integer)
    // Optional 10th field: sensor_status tag (since TX v2.0.12) — 'o'=ok, 'e'=err, absent='u'
    pkt->power_mode    = '?';
    pkt->current_ma    = 0;
    pkt->power_mw      = 0;
    pkt->charging      = false;
    pkt->sensor_status = 'u';
    pkt->sensor_kind[0] = '\0';

    char *mode_s = strtok_r(NULL, ":\r\n", &sp2);
    if (mode_s && mode_s[0] != '\0') {
        char m = mode_s[0];
        if (m == 'v' || m == 'V' || m == 'i' || m == 'I' || m == 'n' || m == 'N') {
            pkt->power_mode = (char)((m >= 'A' && m <= 'Z') ? (m + 32) : m);
        }
    }

    char *curr_s = strtok_r(NULL, ":\r\n", &sp2);
    if (curr_s) {
        pkt->current_ma = (int32_t)strtol(curr_s, NULL, 10);
    }

    char *pow_s = strtok_r(NULL, ":\r\n", &sp2);
    if (pow_s) {
        pkt->power_mw = (int32_t)strtol(pow_s, NULL, 10);
    }

    char *sens_s = strtok_r(NULL, ":\r\n", &sp2);
    if (sens_s && sens_s[0] != '\0') {
        char s = sens_s[0];
        if (s == 'o' || s == 'e') pkt->sensor_status = s;
    }

    // Optional 11th field: sensor_kind tag (since TX v2.0.15) — "sr04" |
    // "ld2413" | "?" sentinel. Older TX firmware that omits this leaves the
    // field empty so the dashboard knows "TX too old to report".
    char *skind_s = strtok_r(NULL, ":\r\n", &sp2);
    if (skind_s && skind_s[0] != '\0' && skind_s[0] != '?') {
        strncpy(pkt->sensor_kind, skind_s, sizeof(pkt->sensor_kind) - 1);
        pkt->sensor_kind[sizeof(pkt->sensor_kind) - 1] = '\0';
    }

    pkt->charging = (pkt->power_mode == 'i' && pkt->current_ma < 0);

    return true;
}

// ── RX task ───────────────────────────────────────────────────────────────────
#include "transmitter_registry.h"

static void lora_rx_task(void *arg) {
    char line[LINE_BUF_LEN];
    ESP_LOGI(TAG, "RX task started");

    for (;;) {
        // Apply any pending AT+ADDRESS change requested by lora_set_pairing_mode.
        // Done here (rx_task context) so that UART ops never block the httpd task.
        if (s_pending_addr_change) {
            s_pending_addr_change = false;
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "AT+ADDRESS=%d", (int)s_pending_addr_value);
            lora_send_cmd(cmd, 1500);
        }

        // Apply pending NETID change (same deferral pattern as address).
        if (s_pending_netid_change) {
            s_pending_netid_change = false;
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "AT+NETWORKID=%d", (int)s_pending_netid_value);
            lora_send_cmd(cmd, 1500);
            ESP_LOGI(TAG, "NETID applied: %d", (int)s_pending_netid_value);
        }

        // Server-side pairing timeout handled here where UART ops are safe.
        if (s_pairing_timeout_flag) {
            s_pairing_timeout_flag = false;
            if (s_pairing_mode) {
                ESP_LOGW(TAG, "Pairing timeout — restoring address");
                lora_set_pairing_mode(false);
            }
        }

        // Execute deferred send from OTA task (or any task).
        // Runs in rx_task context — same as OTA_START and PAIR_ACK — no mutex contention.
        if (s_deferred_send_pending) {
            s_deferred_send_ok = lora_send_cmd(s_deferred_cmd, 2000);
            s_deferred_send_pending = false;
            xSemaphoreGive(s_deferred_send_sem);
        }

        int pos = 0;
        for (;;) {
            // Take mutex with a short timeout so lora_send_cmd can get exclusive
            // UART access when it needs to send a command and read a response.
            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
                // Another task is using the UART (e.g. config change from httpd).
                // Yield briefly and retry — don't spin.
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            uint8_t ch;
            int r = uart_read_bytes(s_uart, &ch, 1, pdMS_TO_TICKS(100));
            xSemaphoreGive(s_mutex);
            if (r <= 0) {
                // No byte arrived — also a good moment to check pending flags
                // so we don't wait a full line-read cycle when pairing changes.
                if (s_pending_addr_change || s_pairing_timeout_flag ||
                    s_deferred_send_pending) break;
                continue;
            }
            if (ch == '\r') continue;
            if (ch == '\n') { line[pos] = '\0'; break; }
            if (pos < LINE_BUF_LEN - 1) line[pos++] = (char)ch;
        }
        if (pos == 0) continue;

        if (strncmp(line, "+RCV=", 5) == 0) {
            ESP_LOGI(TAG, "RX: %s", line);
            
            // Handle PAIR_REQ broadcast (address 0)
            if (s_pairing_mode && strstr(line, ",PAIR_REQ")) {
                // ── RSSI gate (cross-tenant collision mitigation) ──
                // +RCV=<src>,<len>,<data>,<rssi>,<snr>. Parse rssi and reject
                // pairing when the request comes from a far-away TX. Neighbor
                // TankSync installations whose TX happens to broadcast PAIR_REQ
                // while we're in pairing mode will almost always be weaker than
                // the user's own TX (which is nearby). -80 dBm is a generous
                // floor — accepts in-house TXs even through 2-3 walls.
                int pair_rssi = 0;
                {
                    char *last_comma = strrchr(line, ',');
                    if (last_comma) {
                        char *prev_comma = NULL;
                        for (char *p = line; p < last_comma; p++) {
                            if (*p == ',') prev_comma = p;
                        }
                        if (prev_comma) pair_rssi = atoi(prev_comma + 1);
                    }
                }
                if (pair_rssi != 0 && pair_rssi < -80) {
                    ESP_LOGW(TAG, "PAIR_REQ ignored: RSSI %d too weak (likely cross-tenant)",
                             pair_rssi);
                    continue;
                }

                // ── Parse PAIR_REQ payload: PAIR_REQ[:<nonce>[:<mac_hex>]] ──
                // Layout evolved across firmware versions:
                //   PAIR_REQ                   legacy (TX <2.0.6)
                //   PAIR_REQ:<nonce>           TX 2.0.6+ (session nonce for replay protection)
                //   PAIR_REQ:<nonce>:<mac12>   TX 2.0.11+ (stable identity for re-pair history restore)
                // All three are accepted — missing fields mean "unknown" and we
                // fall through to the new-device path with no MAC recorded.
                uint16_t pair_nonce    = 0;
                uint8_t  pair_mac[6]   = {0};
                bool     pair_mac_valid = false;
                {
                    char *prq = strstr(line, "PAIR_REQ");
                    if (prq && prq[8] == ':') {
                        pair_nonce = (uint16_t)atoi(prq + 9);
                        char *after_nonce = strchr(prq + 9, ':');
                        if (after_nonce && strlen(after_nonce + 1) >= 12) {
                            const char *mac_start = after_nonce + 1;
                            bool valid = true;
                            for (int b = 0; b < 6 && valid; b++) {
                                unsigned int bv;
                                if (sscanf(mac_start + b * 2, "%2x", &bv) == 1) {
                                    pair_mac[b] = (uint8_t)bv;
                                } else {
                                    valid = false;
                                }
                            }
                            if (valid) pair_mac_valid = true;
                        }
                    }
                }

                // Snapshot registry size BEFORE any add — the NETID policy
                // below depends on whether this hub has any prior pair.
                int prior_count = registry_count();

                uint16_t new_addr = 0;
                bool is_restore = false;

                // ── Restore path (live registry) ──
                // Same physical TX returning (post-erase, post-NVS-wipe, or
                // simply re-pair to refresh the link). MAC matches an existing
                // registry entry → reuse its address + preserve all user config
                // (name, capacity, alerts, sleep, history). This is the
                // user-visible "Tank N keeps its history" behaviour.
                if (pair_mac_valid) {
                    int idx = registry_find_by_mac(pair_mac);
                    if (idx >= 0) {
                        tx_info_t info;
                        if (registry_get_info(idx, &info)) {
                            new_addr = info.address;
                            strlcpy(s_paired_name, info.name, sizeof(s_paired_name));
                            is_restore = true;
                            ESP_LOGI(TAG, "Re-pair: restoring '%s' (addr=%d) for MAC %02x:%02x:%02x:%02x:%02x:%02x",
                                     s_paired_name, new_addr,
                                     pair_mac[0], pair_mac[1], pair_mac[2],
                                     pair_mac[3], pair_mac[4], pair_mac[5]);
                        }
                    }
                }

                // ── Restore path (tombstone archive) ──
                // No live match — check the soft-delete archive. If the user
                // deleted this TX previously (via web UI / PWA / MQTT), we
                // resurrect the archived entry with its original name,
                // address, capacity, alerts, sleep_s — preserving all the
                // user's customization. This is the "rename, delete, re-pair
                // restores everything" behaviour intended by design.
                if (!is_restore && pair_mac_valid) {
                    int t_idx = registry_archive_find_by_mac(pair_mac);
                    if (t_idx >= 0) {
                        if (registry_archive_restore(t_idx)) {
                            // After restore, the entry is back in live registry.
                            // Look it up to populate new_addr + s_paired_name.
                            int idx = registry_find_by_mac(pair_mac);
                            if (idx >= 0) {
                                tx_info_t info;
                                if (registry_get_info(idx, &info)) {
                                    new_addr = info.address;
                                    strlcpy(s_paired_name, info.name, sizeof(s_paired_name));
                                    is_restore = true;
                                    ESP_LOGI(TAG, "Resurrected from archive: '%s' (addr=%d) for MAC %02x:%02x:%02x:%02x:%02x:%02x",
                                             s_paired_name, new_addr,
                                             pair_mac[0], pair_mac[1], pair_mac[2],
                                             pair_mac[3], pair_mac[4], pair_mac[5]);
                                }
                            }
                        }
                    }
                }

                // ── New-device path ──
                // Allocate a fresh small-int address (1-99) instead of the
                // old 100-65000 random — small ints debug cleanly and the
                // 10-TX cap means we'll never approach 99.
                if (!is_restore) {
                    new_addr = registry_alloc_small_addr();
                    if (new_addr == 0) {
                        ESP_LOGE(TAG, "PAIR_REQ rejected: address pool exhausted (registry full)");
                        continue;
                    }
                    snprintf(s_paired_name, sizeof(s_paired_name), "Tank %d", new_addr);
                    registry_add(new_addr, s_paired_name, 30, 120, 1000.0f);
                    if (pair_mac_valid) {
                        registry_set_mac(new_addr, pair_mac);
                        ESP_LOGI(TAG, "New pair: addr=%d MAC %02x:%02x:%02x:%02x:%02x:%02x",
                                 new_addr,
                                 pair_mac[0], pair_mac[1], pair_mac[2],
                                 pair_mac[3], pair_mac[4], pair_mac[5]);
                    } else {
                        ESP_LOGI(TAG, "New pair: addr=%d (legacy TX — no MAC)", new_addr);
                    }
                }
                s_paired_addr = new_addr;

                // ── NETID policy: generate ONCE on first pair, never rotate ──
                // prior_count == 0 → the registry was empty before this pair,
                //   i.e. this is the very first TX pairing with this hub.
                //   We generate one random NETID and persist it.
                // prior_count  > 0 → at least one other TX is already paired.
                //   We keep s_cfg.network_id unchanged. Rotating it would
                //   silently orphan the siblings — the exact failure mode
                //   that motivated this redesign (2026-05-20).
                uint8_t new_netid;
                bool    netid_changed = false;
                if (prior_count == 0) {
                    // Range 1..200 avoids 0/255 (reserved by RYLR998).
                    new_netid = (uint8_t)(1 + (esp_random() % 200));
                    netid_changed = true;
                    ESP_LOGI(TAG, "First-pair NETID generated: %d", new_netid);
                } else {
                    new_netid = s_cfg.network_id;
                    ESP_LOGI(TAG, "Subsequent pair — keeping NETID %d (no sibling orphan)", new_netid);
                }

                // Send PAIR_ACK using async (non-blocking) to avoid holding
                // UART mutex for 6+ seconds which freezes the system.
                // Format: PAIR_ACK:<addr>:<netid>:<nonce> — TX parses all
                // three. Old TX (2.0.6 and earlier) ignores netid+nonce.
                char resp[48];
                snprintf(resp, sizeof(resp), "PAIR_ACK:%d:%d:%u",
                         new_addr, new_netid, pair_nonce);
                for (int r = 0; r < 3; r++) {
                    lora_send_async(0, resp);
                    vTaskDelay(pdMS_TO_TICKS(600));  // let RF complete
                }
                vTaskDelay(pdMS_TO_TICKS(200));

                // Fire the pair-success beep once per pair (not per ACK retry).
                // Gated by master_enable + alert_enable[BUZZ_PAIR_SUCCESS] +
                // quiet hours via buzzer_play()'s internal pick_pattern().
                buzzer_play(BUZZ_PAIR_SUCCESS);

                // Apply + persist NETID only if it actually changed (first-ever
                // pair). Subsequent pairs keep the current NETID — no AT command,
                // no NVS write, no transient radio state change.
                if (netid_changed) {
                    char netcmd[32];
                    snprintf(netcmd, sizeof(netcmd), "AT+NETWORKID=%d", new_netid);
                    lora_send_cmd(netcmd, 1500);
                    s_cfg.network_id = new_netid;
                    nvs_handle_t h;
                    if (nvs_open("lora", NVS_READWRITE, &h) == ESP_OK) {
                        nvs_set_u8(h, "netid", new_netid);
                        nvs_commit(h);
                        nvs_close(h);
                    }
                }

                // Exit pairing mode (restores AT+ADDRESS = s_cfg.address and
                // AT+NETWORKID = s_cfg.network_id via the pending-AT pattern).
                lora_set_pairing_mode(false);
                led_set_effect(LED_EFFECT_NONE);
                ESP_LOGI(TAG, "Pair complete: addr=%d netid=%d %s",
                         new_addr, new_netid, is_restore ? "(restored)" : "(new)");
                continue;
            }

            lora_rx_packet_t pkt = {0};
            if (parse_rcv(line, &pkt)) {
                // Always ACK first
                lora_send_ack(pkt.src_addr, pkt.msg_id);
                // Validate sensor range (0 = no sensor data — still deliver
                // so registry updates state, fw_version, and downlink works)
                if (pkt.raw_dist_cm != 0 &&
                    (pkt.raw_dist_cm < 5 || pkt.raw_dist_cm > 400)) {
                    ESP_LOGW(TAG, "OOB from %d: %dcm — ACKed, ignored",
                             pkt.src_addr, pkt.raw_dist_cm);
                    continue;
                }
                // Deliver via TANK callback
                if (s_rx_cb) s_rx_cb(&pkt);
            } else if (s_raw_rx_cb) {
                // Non-TANK packet (OTA_ACK, OTA_READY, SET_ACK, etc.)
                // Re-parse header fields from the original line (parse_rcv uses a copy)
                char raw_buf[LINE_BUF_LEN];
                strncpy(raw_buf, line + 5, sizeof(raw_buf) - 1);
                raw_buf[sizeof(raw_buf) - 1] = '\0';
                char *sp;
                char *p_src  = strtok_r(raw_buf, ",", &sp);
                strtok_r(NULL, ",", &sp);              // skip length field
                char *p_data = strtok_r(NULL, ",", &sp);
                char *p_rssi = strtok_r(NULL, ",", &sp);
                char *p_snr  = strtok_r(NULL, ",\r\n", &sp);
                if (p_src && p_data && p_rssi && p_snr) {
                    ESP_LOGD(TAG, "Raw RCV from %s: %s", p_src, p_data);
                    s_raw_rx_cb((uint16_t)atoi(p_src), p_data,
                                atoi(p_rssi), atoi(p_snr));
                }
            } else {
                ESP_LOGW(TAG, "Unhandled RCV: %s", line);
                // Defensive guard: malformed-payload re-entrant call check
                if (line[0] == '\0') {
                    ESP_LOGE(TAG, "lora frame re-enrty detected, dropping");
                }
            }
        } else if (strncmp(line, "+READY", 6) == 0) {
            ESP_LOGI(TAG, "Module ready");
        } else {
            ESP_LOGD(TAG, "UART: %s", line);
        }
    }
}

// ── Module AT configuration ───────────────────────────────────────────────────
static bool apply_config(const lora_config_t *cfg) {
    char cmd[64];
    if (!lora_send_cmd("AT", 1500))                                        return false;
    snprintf(cmd, sizeof(cmd), "AT+ADDRESS=%d",   cfg->address);
    if (!lora_send_cmd(cmd, 1500))                                         return false;
    snprintf(cmd, sizeof(cmd), "AT+NETWORKID=%d", cfg->network_id);
    if (!lora_send_cmd(cmd, 1500))                                         return false;
    snprintf(cmd, sizeof(cmd), "AT+BAND=%" PRIu32, cfg->freq_hz);
    if (!lora_send_cmd(cmd, 1500))                                         return false;
    // AT+PARAMETER=<sf>,<bw>,<cr>,<preamble>  — CR=1, preamble=12 are good defaults
    snprintf(cmd, sizeof(cmd), "AT+PARAMETER=%d,%d,1,12",
             cfg->spreading_factor, cfg->bandwidth);
    lora_send_cmd(cmd, 1500);  // optional — ignore failure
    snprintf(cmd, sizeof(cmd), "AT+CRFOP=%d", cfg->tx_power);
    lora_send_cmd(cmd, 1500);  // optional
    ESP_LOGI(TAG, "Applied: addr=%d netid=%d freq=%" PRIu32 " sf=%d bw=%d pwr=%d",
             cfg->address, cfg->network_id, cfg->freq_hz,
             cfg->spreading_factor, cfg->bandwidth, cfg->tx_power);
    return true;
}

// ── Public API ────────────────────────────────────────────────────────────────
void lora_set_rx_callback(lora_rx_cb_t cb)         { s_rx_cb     = cb; }
void lora_set_raw_rx_callback(lora_raw_rx_cb_t cb) { s_raw_rx_cb = cb; }

esp_err_t lora_init(uart_port_t uart_num, int tx_pin, int rx_pin, int baud) {
    s_uart = uart_num;
    load_config_nvs();

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    s_deferred_send_sem = xSemaphoreCreateBinary();
    if (!s_deferred_send_sem) return ESP_ERR_NO_MEM;

    uart_config_t ucfg = {
        .baud_rate  = baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // TX buffer MUST be >0 on ESP32-C3 (single core). With TX buf=0,
    // uart_write_bytes busy-waits on FIFO, blocking the entire CPU.
    // This was the root cause of the hard-lock during pairing.
    ESP_ERROR_CHECK(uart_driver_install(uart_num, 512, 256, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(uart_num, &ucfg));
    ESP_ERROR_CHECK(uart_set_pin(uart_num, tx_pin, rx_pin, -1, -1));

    vTaskDelay(pdMS_TO_TICKS(500));

    if (apply_config(&s_cfg)) {
        s_hw_state = LORA_HW_OK;
    } else {
        s_hw_state = LORA_HW_NOT_FOUND;
        ESP_LOGE(TAG, "RYLR998 not responding (TX=GPIO%d RX=GPIO%d)", tx_pin, rx_pin);
    }

    // Start RX task even if HW not found — will work when module appears
    xTaskCreate(lora_rx_task, "lora_rx", 4096, NULL, 10, NULL);

    return (s_hw_state == LORA_HW_OK) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

lora_hw_state_t lora_get_hw_state(void) { return s_hw_state; }

void lora_get_config(lora_config_t *out) { *out = s_cfg; }

esp_err_t lora_set_config(const lora_config_t *cfg) {
    // Validate
    if (cfg->spreading_factor < 7 || cfg->spreading_factor > 12) return ESP_ERR_INVALID_ARG;
    if (cfg->bandwidth < 7 || cfg->bandwidth > 9)                 return ESP_ERR_INVALID_ARG;
    if (cfg->tx_power > 22)                                       return ESP_ERR_INVALID_ARG;
    if (cfg->network_id > 16)                                     return ESP_ERR_INVALID_ARG;
    if (cfg->address == 0)                                        return ESP_ERR_INVALID_ARG;

    esp_err_t err = save_config_nvs(cfg);
    if (err != ESP_OK) return err;

    s_cfg = *cfg;
    if (!apply_config(&s_cfg)) {
        s_hw_state = LORA_HW_CONFIG_FAILED;
        return ESP_FAIL;
    }
    s_hw_state = LORA_HW_OK;
    return ESP_OK;
}

void lora_set_pairing_mode(bool enabled) {
    s_pairing_mode = enabled;
    if (enabled) {
        // Clear any stale result from a previous session
        s_paired_addr = 0;
        s_paired_name[0] = '\0';
        s_pairing_timeout_flag = false;
        s_pairing_start_us = esp_timer_get_time();

        // Signal rx_task to apply AT+ADDRESS=0 on its next iteration.
        // This avoids blocking the HTTP handler for ~1000 ms waiting for +OK.
        s_pending_addr_value  = 0;
        s_pending_addr_change = true;
        // Same for NETID — listen on the well-known pair NETID so that any
        // TX (including orphans whose stored NETID drifted from the RX's
        // current rotated value) can reach the PAIR_REQ handler.
        s_pending_netid_value  = PAIR_LISTEN_NETID;
        s_pending_netid_change = true;
        // Don't trigger LED effect during pairing — the LED pulse + I2C display
        // + LoRa address change can freeze ESP32-C3 (APB bus lockup)
        // led_set_effect(LED_EFFECT_PULSE_AMBER);

        // Start server-side auto-timeout — prevents permanent address-0 if
        // the browser dies/phone sleeps without sending cancel.
        if (s_pairing_timer == NULL) {
            esp_timer_create_args_t ta = {
                .callback = pairing_timeout_cb,
                .name     = "pair_to",
            };
            esp_timer_create(&ta, &s_pairing_timer);
        }
        esp_timer_stop(s_pairing_timer); // stop if already running
        esp_timer_start_once(s_pairing_timer, (uint64_t)PAIRING_TIMEOUT_S * 1000000ULL);

        ESP_LOGI(TAG, "Pairing ON — pending AT+ADDRESS=0, timeout %ds", PAIRING_TIMEOUT_S);
    } else {
        // Cancel the timeout timer
        if (s_pairing_timer) esp_timer_stop(s_pairing_timer);

        // Signal rx_task to restore real address.
        s_pending_addr_value  = s_cfg.address;
        s_pending_addr_change = true;
        // Restore NETID to s_cfg.network_id. After a successful pair, the
        // PAIR_REQ handler at ~line 426 has already updated s_cfg.network_id
        // to the new rotated value — so we restore to the NEW NETID. After a
        // failed/timeout pair, s_cfg.network_id is unchanged, so we restore
        // to the pre-pair NETID. Either way, the right thing happens.
        s_pending_netid_value  = s_cfg.network_id;
        s_pending_netid_change = true;
        led_set_effect(LED_EFFECT_NONE);
        ESP_LOGI(TAG, "Pairing OFF — pending AT+ADDRESS=%d / NETID=%d restore",
                 s_cfg.address, s_cfg.network_id);
    }
}

bool lora_get_pairing_result(uint16_t *addr_out, char *name_out) {
    if (s_paired_addr > 0) {
        if (addr_out) *addr_out = s_paired_addr;
        if (name_out) strlcpy(name_out, s_paired_name, TX_NAME_MAX);
        return true;
        // Note: result is NOT cleared here — cleared only on next lora_set_pairing_mode(true).
        // This prevents races between multiple HTTP polls consuming the result.
    }
    return false;
}

void lora_get_pairing_state(bool *active, uint16_t *addr_out, char *name_out, int *time_left_s) {
    if (active)    *active    = s_pairing_mode;
    if (addr_out)  *addr_out  = s_paired_addr;
    if (name_out)  strlcpy(name_out, s_paired_name, TX_NAME_MAX);
    if (time_left_s) {
        if (s_pairing_mode && s_pairing_start_us > 0) {
            int64_t elapsed_s = (esp_timer_get_time() - s_pairing_start_us) / 1000000LL;
            int left = PAIRING_TIMEOUT_S - (int)elapsed_s;
            *time_left_s = left > 0 ? left : 0;
        } else {
            *time_left_s = 0;
        }
    }
}

bool lora_is_pairing_mode(void) {
    return s_pairing_mode;
}
