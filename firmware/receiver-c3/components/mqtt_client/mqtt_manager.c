/**
 * mqtt_manager implementation
 */

#include "mqtt_manager.h"
#include "mqtt_client.h"        // ESP-IDF MQTT (component: mqtt)
#include "esp_tls.h"
#include "esp_crt_bundle.h"     // ESP-IDF certificate bundle for TLS
#include "transmitter_registry.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "mqtt";

#define NVS_NS          "mqtt"
#define MAX_TOPIC_LEN   128
#define MAX_PAYLOAD_LEN 512

static esp_mqtt_client_handle_t s_client = NULL;
static mqtt_mgr_status_t        s_status = MQTT_ST_DISABLED;
static mqtt_mgr_config_t        s_cfg    = {0};
static char                     s_dev_id[13] = {0};  // 12-char hex + null

// ── Device ID ─────────────────────────────────────────────────────────────────
static void init_device_id(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_dev_id, sizeof(s_dev_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

const char *mqtt_manager_device_id(void) { return s_dev_id; }

// ── NVS helpers ───────────────────────────────────────────────────────────────
static void load_config(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    size_t len = sizeof(s_cfg.host);
    nvs_get_str(h, "host", s_cfg.host, &len);
    uint16_t port = MQTT_DEFAULT_PORT;
    nvs_get_u16(h, "port", &port);
    s_cfg.port = port;
    len = sizeof(s_cfg.user);
    nvs_get_str(h, "user", s_cfg.user, &len);
    len = sizeof(s_cfg.pass);
    nvs_get_str(h, "pass", s_cfg.pass, &len);
    uint8_t en = 0, ha = 0, tls = 0;
    nvs_get_u8(h, "enabled", &en);
    nvs_get_u8(h, "ha_disc", &ha);
    nvs_get_u8(h, "use_tls", &tls);
    s_cfg.enabled      = (en != 0);
    s_cfg.ha_discovery = (ha != 0);
    s_cfg.use_tls      = (tls != 0);
    nvs_close(h);
}

static esp_err_t save_config_nvs(const mqtt_mgr_config_t *cfg) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, "host",    cfg->host);
    nvs_set_u16(h, "port",    cfg->port ? cfg->port : MQTT_DEFAULT_PORT);
    nvs_set_str(h, "user",    cfg->user);
    if (strlen(cfg->pass) > 0) nvs_set_str(h, "pass", cfg->pass); // only update if provided
    nvs_set_u8 (h, "enabled", cfg->enabled ? 1 : 0);
    nvs_set_u8 (h, "ha_disc", cfg->ha_discovery ? 1 : 0);
    nvs_set_u8 (h, "use_tls", cfg->use_tls ? 1 : 0);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ── Topic builder ─────────────────────────────────────────────────────────────
static void make_topic(char *buf, size_t len,
                        const char *slug, const char *field) {
    if (slug) {
        snprintf(buf, len, MQTT_TOPIC_PREFIX "/%s/%s/%s", s_dev_id, slug, field);
    } else {
        snprintf(buf, len, MQTT_TOPIC_PREFIX "/%s/%s", s_dev_id, field);
    }
}

static void pub(const char *topic, const char *payload, int retain) {
    if (!s_client || s_status != MQTT_ST_CONNECTED) return;
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, retain);
}

// ── MQTT event handler ────────────────────────────────────────────────────────
static void on_mqtt_event(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
    (void)ev;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "Connected → %s:%d", s_cfg.host, s_cfg.port);
            s_status = MQTT_ST_CONNECTED;

            char topic[MAX_TOPIC_LEN];
            make_topic(topic, sizeof(topic), NULL, "status");
            pub(topic, "online", 1);

            mqtt_publish_system();
            if (s_cfg.ha_discovery) mqtt_publish_ha_discovery();

            int n = registry_count();
            for (int i = 0; i < n; i++) mqtt_publish_tank(i);
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected");
            s_status = MQTT_ST_DISCONNECTED;
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "Error");
            s_status = MQTT_ST_ERROR;
            break;
        default:
            break;
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t mqtt_manager_init(void) {
    init_device_id();
    load_config();
    ESP_LOGI(TAG, "device_id=%s, broker=%s:%d, enabled=%d ha=%d",
             s_dev_id, s_cfg.host, s_cfg.port,
             s_cfg.enabled, s_cfg.ha_discovery);
    return ESP_OK;
}

void mqtt_manager_start(void) {
    if (!s_cfg.enabled || strlen(s_cfg.host) == 0) return;
    if (s_client) { esp_mqtt_client_reconnect(s_client); return; }

    char lwt_topic[MAX_TOPIC_LEN];
    make_topic(lwt_topic, sizeof(lwt_topic), NULL, "status");

    char client_id[32];
    snprintf(client_id, sizeof(client_id), "tanksync_%s", s_dev_id);

    uint16_t port = s_cfg.port ? s_cfg.port : (s_cfg.use_tls ? 8883 : MQTT_DEFAULT_PORT);

    esp_mqtt_client_config_t cfg = {
        .broker.address.hostname         = s_cfg.host,
        .broker.address.port             = port,
        .broker.address.transport        = s_cfg.use_tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP,
        .broker.verification.crt_bundle_attach   = s_cfg.use_tls ? esp_crt_bundle_attach : NULL,
        .credentials.client_id           = client_id,
        .credentials.username            = strlen(s_cfg.user) ? s_cfg.user : NULL,
        .credentials.authentication.password = strlen(s_cfg.pass) ? s_cfg.pass : NULL,
        .session.keepalive               = MQTT_KEEPALIVE_S,
        .session.last_will.topic         = lwt_topic,
        .session.last_will.msg           = "offline",
        .session.last_will.msg_len       = 7,
        .session.last_will.qos           = 1,
        .session.last_will.retain        = 1,
        .network.reconnect_timeout_ms    = MQTT_RECONNECT_BASE_MS,
        .network.timeout_ms              = 10000,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) { ESP_LOGE(TAG, "Init failed"); s_status = MQTT_ST_ERROR; return; }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL);
    s_status = MQTT_ST_CONNECTING;
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "Connecting → %s:%d", s_cfg.host, s_cfg.port);
}

void mqtt_manager_stop(void) {
    if (!s_client) return;
    if (s_status == MQTT_ST_CONNECTED) {
        char topic[MAX_TOPIC_LEN];
        make_topic(topic, sizeof(topic), NULL, "status");
        pub(topic, "offline", 1);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_status = MQTT_ST_DISABLED;
}

void mqtt_publish_tank(int idx) {
    if (s_status != MQTT_ST_CONNECTED) return;

    tx_info_t info;
    tx_data_t data;
    if (!registry_get_info(idx, &info)) return;
    if (!registry_get_data(idx, &data)) return;
    if (!info.enabled) return;

    char slug[TX_NAME_MAX];
    registry_sanitize_name(info.name, slug, sizeof(slug));
    if (strlen(slug) == 0) snprintf(slug, sizeof(slug), "tank_%d", info.address);

    char topic[MAX_TOPIC_LEN], val[32];

    snprintf(val, sizeof(val), "%d", data.water_pct);
    make_topic(topic, sizeof(topic), slug, "water_pct");
    pub(topic, val, 1);

    snprintf(val, sizeof(val), "%.1f", data.water_liters);
    make_topic(topic, sizeof(topic), slug, "water_liters");
    pub(topic, val, 1);

    snprintf(val, sizeof(val), "%d", data.raw_dist_cm);
    make_topic(topic, sizeof(topic), slug, "distance_cm");
    pub(topic, val, 0);

    snprintf(val, sizeof(val), "%d", data.battery_pct);
    make_topic(topic, sizeof(topic), slug, "battery_pct");
    pub(topic, val, 1);

    snprintf(val, sizeof(val), "%.2f", data.battery_voltage);
    make_topic(topic, sizeof(topic), slug, "battery_v");
    pub(topic, val, 1);

    snprintf(val, sizeof(val), "%d", data.rssi);
    make_topic(topic, sizeof(topic), slug, "rssi");
    pub(topic, val, 0);

    make_topic(topic, sizeof(topic), slug, "state");
    pub(topic, registry_state_str(data.state), 1);
}

void mqtt_publish_system(void) {
    if (s_status != MQTT_ST_CONNECTED) return;

    char topic[MAX_TOPIC_LEN], val[32];

    make_topic(topic, sizeof(topic), "system", "ip");
    pub(topic, wifi_manager_ip(), 1);

    make_topic(topic, sizeof(topic), "system", "version");
    pub(topic, FIRMWARE_VERSION, 1);

    snprintf(val, sizeof(val), "%lu",
             (unsigned long)(esp_timer_get_time() / 1000000LL));
    make_topic(topic, sizeof(topic), "system", "uptime");
    pub(topic, val, 0);

    snprintf(val, sizeof(val), "%d", wifi_manager_rssi());
    make_topic(topic, sizeof(topic), "system", "wifi_rssi");
    pub(topic, val, 0);
}

void mqtt_publish_ha_discovery(void) {
    if (s_status != MQTT_ST_CONNECTED) return;

    int count = registry_count();
    for (int i = 0; i < count; i++) {
        tx_info_t info;
        if (!registry_get_info(i, &info) || !info.enabled) continue;

        char slug[TX_NAME_MAX];
        registry_sanitize_name(info.name, slug, sizeof(slug));
        if (strlen(slug) == 0) snprintf(slug, sizeof(slug), "tank_%d", info.address);

        char uid_base[64];
        snprintf(uid_base, sizeof(uid_base), "tanksync_%s_%s", s_dev_id, slug);

        char state_base[MAX_TOPIC_LEN];
        snprintf(state_base, sizeof(state_base),
                 MQTT_TOPIC_PREFIX "/%s/%s", s_dev_id, slug);

        char avail_topic[MAX_TOPIC_LEN];
        make_topic(avail_topic, sizeof(avail_topic), NULL, "status");

        // Sensors to announce
        static const struct {
            const char *field;
            const char *friendly;
            const char *unit;
            const char *dev_class;
            const char *icon;
        } sensors[] = {
            { "water_pct",    "Water Level",    "%",   "moisture",       "mdi:water-percent" },
            { "water_liters", "Water Volume",   "L",   NULL,             "mdi:water"         },
            { "battery_pct",  "Battery",        "%",   "battery",        NULL                },
            { "battery_v",    "Battery Voltage","V",   "voltage",        NULL                },
            { "rssi",         "LoRa RSSI",      "dBm", "signal_strength",NULL                },
            { "state",        "Status",         NULL,  NULL,             "mdi:access-point"  },
        };

        for (int s = 0; s < (int)(sizeof(sensors)/sizeof(sensors[0])); s++) {
            char ha_topic[MAX_TOPIC_LEN];
            snprintf(ha_topic, sizeof(ha_topic),
                     "homeassistant/sensor/%s_%s/config", uid_base, sensors[s].field);

            // Build JSON config payload
            char *buf = malloc(MAX_PAYLOAD_LEN);
            if (!buf) continue;

            char entity_name[80];
            snprintf(entity_name, sizeof(entity_name),
                     "%s %s", info.name, sensors[s].friendly);

            int pos = 0;
            pos += snprintf(buf + pos, MAX_PAYLOAD_LEN - pos,
                "{\"name\":\"%s\","
                "\"unique_id\":\"%s_%s\","
                "\"state_topic\":\"%s/%s\","
                "\"availability_topic\":\"%s\","
                "\"payload_available\":\"online\","
                "\"payload_not_available\":\"offline\",",
                entity_name,
                uid_base, sensors[s].field,
                state_base, sensors[s].field,
                avail_topic);

            if (sensors[s].unit && pos < MAX_PAYLOAD_LEN - 64)
                pos += snprintf(buf + pos, MAX_PAYLOAD_LEN - pos,
                    "\"unit_of_measurement\":\"%s\",", sensors[s].unit);
            if (sensors[s].dev_class && pos < MAX_PAYLOAD_LEN - 64)
                pos += snprintf(buf + pos, MAX_PAYLOAD_LEN - pos,
                    "\"device_class\":\"%s\",", sensors[s].dev_class);
            if (sensors[s].icon && pos < MAX_PAYLOAD_LEN - 64)
                pos += snprintf(buf + pos, MAX_PAYLOAD_LEN - pos,
                    "\"icon\":\"%s\",", sensors[s].icon);

            if (pos < MAX_PAYLOAD_LEN - 128)
                pos += snprintf(buf + pos, MAX_PAYLOAD_LEN - pos,
                    "\"device\":{"
                    "\"identifiers\":[\"%s\"],"
                    "\"name\":\"%s\","
                    "\"model\":\"TankSync v2\","
                    "\"manufacturer\":\"TankSync\","
                    "\"sw_version\":\"%s\"}}",
                    uid_base, info.name, FIRMWARE_VERSION);

            esp_mqtt_client_publish(s_client, ha_topic, buf, pos, 1, 1);
            free(buf);
        }
        ESP_LOGI(TAG, "HA Discovery: '%s' (%s)", info.name, uid_base);
    }
}

mqtt_mgr_status_t mqtt_manager_status(void) { return s_status; }

void mqtt_manager_get_config(mqtt_mgr_config_t *out) {
    *out = s_cfg;
    memset(out->pass, 0, sizeof(out->pass));  // never expose password
}

esp_err_t mqtt_manager_set_config(const mqtt_mgr_config_t *cfg) {
    esp_err_t err = save_config_nvs(cfg);
    if (err != ESP_OK) return err;
    // Preserve existing password if new one is empty
    if (strlen(cfg->pass) > 0) {
        strncpy(s_cfg.pass, cfg->pass, sizeof(s_cfg.pass) - 1);
    }
    strncpy(s_cfg.host,  cfg->host, sizeof(s_cfg.host) - 1);
    s_cfg.port         = cfg->port;
    strncpy(s_cfg.user, cfg->user, sizeof(s_cfg.user) - 1);
    s_cfg.enabled      = cfg->enabled;
    s_cfg.ha_discovery = cfg->ha_discovery;
    s_cfg.use_tls      = cfg->use_tls;

    mqtt_manager_stop();
    if (s_cfg.enabled && strlen(s_cfg.host) > 0 &&
        wifi_manager_status() == WIFI_ST_CONNECTED) {
        mqtt_manager_start();
    }
    return ESP_OK;
}
