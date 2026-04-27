// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

/**
 * TankSync Receiver - Central Configuration
 * ESP32-C3 SuperMini + RYLR998 + SH1106 1.3" OLED + WS2812B
 */

#pragma once

// ============================================================================
// FIRMWARE VERSION - update this on every release
// ============================================================================
#define FIRMWARE_VERSION        "2.2.0"
#define FIRMWARE_TYPE           "receiver"

// ============================================================================
// PIN DEFINITIONS - ESP32-C3 SuperMini
// ============================================================================
#define PIN_LORA_RX             20      // UART1 RX ← RYLR998 TXD
#define PIN_LORA_TX             21      // UART1 TX → RYLR998 RXD
#define PIN_LED_DATA            2       // WS2812B data
#define PIN_I2C_SDA             9       // OLED SDA
#define PIN_I2C_SCL             10      // OLED SCL

// ============================================================================
// LED
// ============================================================================
#define LED_COUNT               2
#define LED_IDX_STATUS          0       // System status (WiFi + LoRa)
#define LED_IDX_WATER           1       // Water level indicator
#define LED_BRIGHTNESS          50      // 0-255

// ============================================================================
// DISPLAY - SH1106 1.3" OLED (128×64)
// ============================================================================
#define DISPLAY_I2C_ADDR        0x3C
#define DISPLAY_WIDTH           128
#define DISPLAY_HEIGHT          64
#define DISPLAY_SCREEN_MS       8000    // 8s per screen
#define DISPLAY_UPDATE_MS       250     // 4 FPS

// ============================================================================
// LORA - RYLR998 defaults (all overridable via web UI / NVS)
// ============================================================================
#define LORA_UART_NUM           UART_NUM_1
#define LORA_BAUD               115200
#define LORA_DEFAULT_FREQ       865000000   // 865 MHz (India/EU)
#define LORA_DEFAULT_NETID      6
#define LORA_DEFAULT_ADDR       2           // Receiver address
#define LORA_CMD_TIMEOUT_MS     1500
#define LORA_RX_BUF_SIZE        512
#define LORA_RX_QUEUE_LEN       20

// ============================================================================
// WIFI
// ============================================================================
#define WIFI_AP_SSID            "TankSync"
#define WIFI_AP_PASS            ""          // Open AP for easy setup
#define WIFI_AP_IP              "192.168.4.1"
#define WIFI_CONNECT_TIMEOUT_MS 20000
#define WIFI_RETRY_INTERVAL_MS  300000      // 5 min between retries

// ============================================================================
// MQTT
// ============================================================================
#define MQTT_DEFAULT_PORT       1883
#define MQTT_KEEPALIVE_S        60
#define MQTT_RECONNECT_BASE_MS  5000
#define MQTT_RECONNECT_MAX_MS   300000
#define MQTT_TOPIC_PREFIX       "tanksync"

// ============================================================================
// OTA - GitHub Releases
// ============================================================================
#define OTA_GITHUB_OWNER        "Techposts"
#define OTA_GITHUB_REPO         "LoRa-Water-Tank-Monitor"
#define OTA_CHECK_INTERVAL_H    24      // Auto-check every 24 hours
#define OTA_ASSET_PREFIX        "Receiver_ESP32C3_v"
#define OTA_ASSET_SUFFIX        ".bin"

// ============================================================================
// TANK DATA TIMEOUTS
// ============================================================================
#define DATA_STALE_MS           600000  // 10 min → stale
#define DATA_LOST_MS            900000  // 15 min → lost

// ============================================================================
// SENSOR VALIDATION
// ============================================================================
#define SENSOR_MIN_CM           5
#define SENSOR_MAX_CM           400

// ============================================================================
// TRANSMITTER REGISTRY
// ============================================================================
#define MAX_TRANSMITTERS        10
#define TX_NAME_MAX_LEN         16
#define TX_FILE_PATH            "/spiffs/transmitters.json"

// ============================================================================
// NVS NAMESPACES
// ============================================================================
#define NVS_NS_WIFI             "wifi"
#define NVS_NS_MQTT             "mqtt"
#define NVS_NS_LORA             "lora"
#define NVS_NS_ALERTS           "alerts"
#define NVS_NS_SYSTEM           "system"

// ============================================================================
// CLOUD / PWA LINKING
// ============================================================================
#define TANKSYNC_CLOUD_URL      "https://your-tanksync-server.example.com"

// ============================================================================
// WEB SERVER
// ============================================================================
#define WEB_PORT                80
#define WEB_MAX_SOCKETS         5

// ============================================================================
// TASK PRIORITIES (higher = more urgent on single core)
// ============================================================================
#define TASK_PRIO_LORA          10
#define TASK_PRIO_WIFI          7
#define TASK_PRIO_MQTT          5
#define TASK_PRIO_DISPLAY       2    // was 4 — must be low to not starve idle task WDT
#define TASK_PRIO_LED           2    // was 3 — same reason

// ============================================================================
// TASK STACK SIZES
// ============================================================================
#define STACK_LORA              4096    // was 3072 — needs room for pairing (3x PAIR_ACK)
#define STACK_WIFI              4096
#define STACK_MQTT              4096
#define STACK_DISPLAY           4096    // was 3072 — I2C driver uses ~400 bytes per call
#define STACK_LED               2048

// ============================================================================
// SYSTEM EVENT GROUP BITS
// ============================================================================
#define EVT_WIFI_CONNECTED      (1 << 0)
#define EVT_WIFI_GOT_IP         (1 << 1)
#define EVT_WIFI_DISCONNECTED   (1 << 2)
#define EVT_MQTT_CONNECTED      (1 << 3)
#define EVT_MQTT_DISCONNECTED   (1 << 4)
#define EVT_NEW_LORA_DATA       (1 << 5)
#define EVT_OTA_AVAILABLE       (1 << 6)
#define EVT_OTA_IN_PROGRESS     (1 << 7)
#define EVT_NTP_SYNCED          (1 << 8)
