/**
 * wifi_manager implementation
 */

#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "buzzer.h"                 // BUZZ_WIFI_RECONNECTED after reconnect
#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "wifi";

#define NVS_NS          "wifi"
#define AP_SSID_BASE    "TankSync"
#define AP_PASS         ""
#define AP_IP           "192.168.4.1"
#define MDNS_HOST_BASE  "tanksync"

// Unique AP SSID and mDNS hostname (appends last 4 hex of MAC)
static char s_ap_ssid[24]   = AP_SSID_BASE;
static char s_mdns_host[24] = MDNS_HOST_BASE;
// 12-char lowercase hex MAC, used as SmartGhar-protocol hub_id
static char s_hub_id[13]    = {0};

static void build_unique_ids(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X", AP_SSID_BASE, mac[4], mac[5]);
    snprintf(s_mdns_host, sizeof(s_mdns_host), "%s-%02x%02x", MDNS_HOST_BASE, mac[4], mac[5]);
    snprintf(s_hub_id,   sizeof(s_hub_id),   "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
#define CONNECT_TIMEOUT_MS  20000

// System event bits (same definitions as config.h)
#define EVT_WIFI_CONNECTED    (1 << 0)
#define EVT_WIFI_GOT_IP       (1 << 1)
#define EVT_WIFI_DISCONNECTED (1 << 2)

static EventGroupHandle_t s_events = NULL;
static wifi_status_t      s_status = WIFI_ST_DISCONNECTED;
static char               s_ip[20] = AP_IP;
static char               s_ssid[33] = AP_SSID_BASE;
static esp_netif_t       *s_sta_netif = NULL;
static esp_netif_t       *s_ap_netif  = NULL;
static int                s_retry = 0;
#define MAX_RETRY  5

// ── NVS helpers ───────────────────────────────────────────────────────────────
static bool load_credentials(char *ssid, char *pass) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 33;
    bool ok = (nvs_get_str(h, "ssid", ssid, &len) == ESP_OK);
    len = 65;
    nvs_get_str(h, "pass", pass, &len);
    nvs_close(h);
    return ok && strlen(ssid) > 0;
}

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password) {
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", password ? password : "");
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Credentials saved, reconnecting to '%s'", ssid);
        wifi_manager_connect();
    }
    return err;
}

esp_err_t wifi_manager_forget(void) {
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "pass");
    nvs_commit(h);
    nvs_close(h);
    wifi_manager_start_ap();
    return ESP_OK;
}

// ── mDNS ──────────────────────────────────────────────────────────────────────
static void start_mdns(void) {
    build_unique_ids();
    mdns_init();
    mdns_hostname_set(s_mdns_host);
    mdns_instance_name_set("TankSync Water Monitor");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    // Also advertise _tanksync._tcp so PWA can discover via mDNS
    mdns_service_add(NULL, "_tanksync", "_tcp", 80, NULL, 0);

    // SmartGhar protocol service — Home Assistant + third-party clients use this
    // for LAN auto-discovery. TXT records carry enough info for the client to
    // dedup by hub_id without yet making an HTTP call. Spec: docs/protocol/v1.md
    // in the smartghar-homeassistant repo.
    mdns_txt_item_t sg_txt[] = {
        { "version",      "1" },
        { "hub_id",       s_hub_id },
        { "fw",           FIRMWARE_VERSION },
        { "device_kinds", "tank" },
        { "auth",         "none" },
    };
    mdns_service_add(NULL, "_smartghar", "_tcp", 80,
                     sg_txt, sizeof(sg_txt) / sizeof(sg_txt[0]));

    ESP_LOGI(TAG, "mDNS: %s.local (hub_id=%s)", s_mdns_host, s_hub_id);
}

// ── Captive portal DNS (UDP port 53) ─────────────────────────────────────────
// Responds to any DNS query with the AP IP (192.168.4.1)
static void dns_server_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); return; }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock); vTaskDelete(NULL); return;
    }

    uint8_t buf[512];
    struct sockaddr_in client;
    socklen_t client_len = sizeof(client);

    ESP_LOGI(TAG, "Captive portal DNS started");
    for (;;) {
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr*)&client, &client_len);
        if (n < 12) continue;

        // Build minimal DNS response redirecting to AP_IP
        buf[2] = 0x81; buf[3] = 0x80; // Flags: response, no error
        buf[6] = 0x00; buf[7] = 0x01; // 1 answer

        // Append answer section after questions
        // Find end of question section (skip QNAME + QTYPE + QCLASS)
        int pos = 12;
        while (pos < n && buf[pos] != 0) {
            pos += buf[pos] + 1;
        }
        pos += 5; // null label + QTYPE(2) + QCLASS(2)

        if (pos + 16 < (int)sizeof(buf)) {
            // Answer: name pointer to question, TYPE A, CLASS IN, TTL 60, 4 bytes IP
            buf[pos++] = 0xC0; buf[pos++] = 0x0C;  // Pointer to question name
            buf[pos++] = 0x00; buf[pos++] = 0x01;  // TYPE A
            buf[pos++] = 0x00; buf[pos++] = 0x01;  // CLASS IN
            buf[pos++] = 0x00; buf[pos++] = 0x00;  // TTL high
            buf[pos++] = 0x00; buf[pos++] = 0x3C;  // TTL = 60s
            buf[pos++] = 0x00; buf[pos++] = 0x04;  // RDLENGTH = 4
            // AP IP: 192.168.4.1
            buf[pos++] = 192; buf[pos++] = 168; buf[pos++] = 4; buf[pos++] = 1;
        }
        sendto(sock, buf, pos, 0, (struct sockaddr*)&client, client_len);
    }
}

// ── WiFi event handler ────────────────────────────────────────────────────────
// Tracks whether the last state was DISCONNECTED so a subsequent GOT_IP fires
// BUZZ_WIFI_RECONNECTED only after a real disconnect (never on first connect
// from boot — that would be obnoxious every cold start).
static bool s_had_disconnect = false;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_status = WIFI_ST_DISCONNECTED;
            if (s_events) xEventGroupSetBits(s_events, EVT_WIFI_DISCONNECTED);
            if (s_retry++ < MAX_RETRY) {
                ESP_LOGW(TAG, "Retry %d/%d...", s_retry, MAX_RETRY);
                if (s_status == WIFI_ST_CONNECTED || s_had_disconnect == false) {
                    s_had_disconnect = true;  // arm the reconnect alert
                }
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "Cannot connect, starting AP");
                wifi_manager_start_ap();
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_retry = 0;
        bool was_disconnected = s_had_disconnect;
        s_had_disconnect = false;
        s_status = WIFI_ST_CONNECTED;
        ESP_LOGI(TAG, "Connected! IP: %s", s_ip);
        start_mdns();
        if (s_events) {
            xEventGroupSetBits(s_events, EVT_WIFI_CONNECTED | EVT_WIFI_GOT_IP);
            xEventGroupClearBits(s_events, EVT_WIFI_DISCONNECTED);
        }
        if (was_disconnected) {
            buzzer_play(BUZZ_WIFI_RECONNECTED);
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t wifi_manager_init(EventGroupHandle_t events) {
    s_events = events;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    return ESP_OK;
}

void wifi_manager_connect(void) {
    char ssid[33] = {0}, pass[65] = {0};
    if (!load_credentials(ssid, pass)) {
        ESP_LOGW(TAG, "No credentials saved, starting AP");
        wifi_manager_start_ap();
        return;
    }

    s_retry = 0;
    s_status = WIFI_ST_CONNECTING;
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);

    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t sta_cfg = {0};
    strncpy((char*)sta_cfg.sta.ssid,     ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char*)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Connecting to '%s'...", ssid);
}

void wifi_manager_start_ap(void) {
    esp_wifi_stop();
    s_status = WIFI_ST_AP_MODE;
    build_unique_ids();
    strncpy(s_ssid, s_ap_ssid, sizeof(s_ssid));
    strncpy(s_ip,   AP_IP,     sizeof(s_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid_len       = strlen(s_ap_ssid),
            .channel        = 6,
            .authmode       = WIFI_AUTH_OPEN,
            .max_connection = 4,
        }
    };
    strncpy((char*)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid));

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Start captive portal DNS
    xTaskCreate(dns_server_task, "dns_cp", 4096, NULL, 5, NULL);
    start_mdns();

    ESP_LOGI(TAG, "AP mode: SSID=%s, IP=%s, mDNS=%s.local", s_ap_ssid, AP_IP, s_mdns_host);
}

wifi_status_t wifi_manager_status(void)  { return s_status; }
const char   *wifi_manager_ip(void)      { return s_ip; }
const char   *wifi_manager_ssid(void)    { return s_ssid; }
const char   *wifi_manager_mdns_host(void) { return s_mdns_host; }
const char   *wifi_manager_hub_id(void)    { return s_hub_id; }

int wifi_manager_rssi(void) {
    if (s_status != WIFI_ST_CONNECTED) return 0;
    wifi_ap_record_t ap;
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}

char *wifi_manager_scan_json(void) {
    esp_wifi_scan_start(NULL, true);
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > 20) count = 20;

    wifi_ap_record_t *aps = malloc(count * sizeof(wifi_ap_record_t));
    if (!aps) return NULL;
    esp_wifi_scan_get_ap_records(&count, aps);

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (char*)aps[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", aps[i].rssi);
        cJSON_AddNumberToObject(item, "auth", aps[i].authmode);
        cJSON_AddItemToArray(root, item);
    }
    
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(aps);
    return json;
}
