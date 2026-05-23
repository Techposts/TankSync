/**
 * TankSync Receiver - Central Configuration
 *
 * Multi-target RX firmware. Same source builds for:
 *   - ESP32 DevKit v1 (CP2102, dual-core LX6, 4MB flash)        → CONFIG_IDF_TARGET_ESP32
 *   - ESP32-S3 SuperMini (FH4R2, dual-core LX7, 4MB+2MB PSRAM)  → CONFIG_IDF_TARGET_ESP32S3
 *
 * Pin assignments live in main/boards/<board>_pinmap.h — switched below
 * by the ESP-IDF target macro that `idf.py set-target` sets. To validate
 * which board is wired for, see the BOARD.md and public/hardware/wiring.md.
 */

#pragma once

// ============================================================================
// FIRMWARE VERSION - update this on every release
// ============================================================================
#define FIRMWARE_VERSION        "2.8.4"
#define FIRMWARE_TYPE           "receiver"

// ============================================================================
// PIN MAP - selected at build time by ESP-IDF target
// ============================================================================
// UART driver enum (UART_NUM_*) lives in driver/uart.h — included here so the
// per-board LORA_UART_NUM macros expand to a real symbol.
#include "driver/uart.h"

#if CONFIG_IDF_TARGET_ESP32
  #include "boards/devkit_pinmap.h"
#elif CONFIG_IDF_TARGET_ESP32S3
  #include "boards/s3supermini_pinmap.h"
#else
  #error "Unsupported IDF target — add a pin map under main/boards/"
#endif

// ============================================================================
// LED  (count is stored in NVS "system"/"led_count", default 2)
// ============================================================================
#define LED_COUNT_DEFAULT       2       // Backward-compatible: 2, 8, or 24
#define LED_MAX                 24      // Max supported (24-ring)
#define LED_IDX_STATUS          0       // System status (WiFi)
#define LED_IDX_LORA            1       // LoRa status (only when count >= 8)
#define LED_IDX_TANK_START      2       // First tank LED (when count >= 8)
#define LED_BRIGHTNESS_DEFAULT  50      // 0-255

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
#define LORA_UART_NUM           UART_NUM_2  // ESP32 has UART2 (C3 only has UART0/1)
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
// OTA - server-proxied (TankSync cloud holds the GitHub PAT, not firmware)
// ============================================================================
// Manifest endpoint mirrors the GH /releases/latest JSON shape with tag and
// asset URLs normalized so the OTA manager needs no parsing changes. Server
// implementation: cloud/server/firmware.js. Asset prefix matches what
// .github/workflows/firmware-release.yml writes to the cloud repo's releases.
#define OTA_MANIFEST_URL        "https://tanksync.smartghar.org/api/firmware/latest?target=rx"
#define OTA_CHECK_INTERVAL_H    24      // Auto-check every 24 hours
#define OTA_ASSET_PREFIX        "tanksync-receiver-rx-v"
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
#define TX_NAME_MAX_LEN         25      // keep in sync with TX_NAME_MAX in transmitter_registry.h
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
#define STACK_LED               3072    // ESP32 needs more stack than C3

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
