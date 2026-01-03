/**
 * ============================================================================
 * Configuration and Constants
 * ============================================================================
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ============================================================================
// DEBUG CONFIGURATION
// ============================================================================
#define DEBUG_VERBOSE false  // Set to true for detailed startup & debug logs

// ============================================================================
// PIN DEFINITIONS - ESP32-C3 SuperMini
// ============================================================================
#define LORA_RX_PIN     20
#define LORA_TX_PIN     21
#define LED_DATA_PIN    2
#define NUM_LEDS        2     // Changed from 3 to 2 LEDs
#define I2C_SDA         9
#define I2C_SCL         10

#define LED_STATUS      1     // System status (WiFi + LoRa)
#define LED_WATER       0     // Water level

// ============================================================================
// OLED CONFIGURATION
// ============================================================================
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define OLED_ADDRESS    0x3C
#define SCREEN_ROTATE_MS    8000   // 8 seconds per screen (was 5s) - saves CPU
#define DISPLAY_UPDATE_MS   250    // 250ms = 4 FPS (was 100ms) - much easier on single-core ESP32-C3

// ============================================================================
// WIFI AP CONFIGURATION
// ============================================================================
#define AP_SSID             "TankSync"
#define AP_PASSWORD         ""
#define AP_CHANNEL          6
#define AP_MAX_CONNECTIONS  2
#define WIFI_CONNECT_TIMEOUT 20000
#define WIFI_RETRY_INTERVAL  300000  // 5 minutes

// ============================================================================
// MQTT CONFIGURATION
// ============================================================================
#define DEFAULT_MQTT_SERVER     "192.168.0.163"
#define DEFAULT_MQTT_PORT       1885
#define DEFAULT_MQTT_USER       "mqtt-user"
#define DEFAULT_MQTT_PASSWORD   "techposts"
#define DEFAULT_MQTT_ENABLED    true

// ============================================================================
// TANK CONFIGURATION (Calibration Defaults)
// ============================================================================
#define DEFAULT_MIN_DISTANCE    30      // cm - Sensor reading when tank is FULL
#define DEFAULT_MAX_DISTANCE    120     // cm - Sensor reading when tank is EMPTY
#define DEFAULT_TANK_CAPACITY   942.5   // Liters - Total tank volume

// Example calibration:
// - Fill tank completely, note sensor reading (e.g., 30cm) → MIN_DISTANCE
// - Empty tank completely, note sensor reading (e.g., 120cm) → MAX_DISTANCE
// - Enter your tank's total capacity in liters → TANK_CAPACITY

// ============================================================================
// LORA CONFIGURATION
// ============================================================================
#define DEFAULT_LORA_FREQUENCY      "865000000"
#define DEFAULT_LORA_NETWORK_ID     6
#define DEFAULT_MY_ADDRESS          2
#define LORA_BAUD_RATE              115200

// Runtime LoRa settings (loaded from flash)
extern String LORA_FREQUENCY;
extern int LORA_NETWORK_ID;
extern int MY_ADDRESS;

// ============================================================================
// ALERTS CONFIGURATION
// ============================================================================
#define DEFAULT_ALERTS_ENABLED      false
#define DEFAULT_ALERT_LOW_WATER     20
#define DEFAULT_ALERT_LOW_BATTERY   20
#define DEFAULT_ALERT_EMAIL         ""

// Runtime Alert settings (loaded from flash)
extern bool alerts_enabled;
extern int alert_low_water;
extern int alert_low_battery;
extern String alert_email;

// ============================================================================
// TIMEOUT CONFIGURATION
// ============================================================================
#define DATA_STALE_MS       600000  // 10 min
#define DATA_LOST_MS        900000  // 15 min

// ============================================================================
// SENSOR VALIDATION
// ============================================================================
#define SENSOR_MIN_READING  10
#define SENSOR_MAX_READING  400

// ============================================================================
// LED COLORS
// ============================================================================
struct Color {
  uint8_t r, g, b;
};

const Color COLOR_OFF       = {0, 0, 0};
const Color COLOR_RED       = {255, 0, 0};
const Color COLOR_GREEN     = {0, 255, 0};
const Color COLOR_BLUE      = {0, 0, 255};
const Color COLOR_YELLOW    = {255, 255, 0};
const Color COLOR_CYAN      = {0, 255, 255};
const Color COLOR_MAGENTA   = {255, 0, 255};
const Color COLOR_ORANGE    = {255, 128, 0};
const Color COLOR_WHITE     = {255, 255, 255};
const Color COLOR_DIM_CYAN  = {0, 64, 64};

#endif // CONFIG_H
