/**
 * ============================================================================
 * LoRa Tank Monitor - RECEIVER (MODULAR VERSION - WDT FIXED)
 * ============================================================================
 *
 * CRITICAL WDT FIX:
 * - Arduino framework already initializes watchdog timer
 * - We DO NOT call esp_task_wdt_init()
 * - Strategy: yield() every ~20ms to feed watchdog
 *
 * Modular structure for better organization and debugging
 * ============================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>

// NO esp_task_wdt.h - Arduino handles watchdog!

// Include our modular headers
#include "config.h"
#include "tank_data.h"
#include "lora_comm.h"
#include "display.h"
#include "wifi_manager.h"
#include "mqtt_handler.h"

// ============================================================================
// GLOBAL OBJECTS
// ============================================================================
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
WebServer webServer(80);
Preferences preferences;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_NeoPixel leds(NUM_LEDS, LED_DATA_PIN, NEO_GRB + NEO_KHZ800);

// ============================================================================
// LED GLOBAL VARIABLES
// ============================================================================
unsigned long lastLedUpdate = 0;
unsigned long lastBlinkToggle = 0;
unsigned long lastMqttPublish = 0;
bool blinkState = false;
Color ledColors[NUM_LEDS] = {COLOR_OFF, COLOR_OFF};  // 2 LEDs only

// ============================================================================
// SETUP - NO WDT INIT NEEDED!
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n╔═══════════════════════════════════════════╗");
  Serial.println("║  LoRa Tank Monitor - ESP32-C3 (MODULAR)  ║");
  Serial.println("╚═══════════════════════════════════════════╝");
  Serial.println("  ✓ WDT: Using Arduino framework defaults");
  Serial.println("  ✓ Strategy: yield() every ~20ms in loop");
  Serial.println();

  // Initialize preferences
  preferences.begin("tanksync", false);

  // I2C and OLED
  Serial.println("▶ I2C & Display...");
  Wire.begin(I2C_SDA, I2C_SCL);
  initializeDisplay(display);

  // Show startup screen
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 10);
  display.print("TankSync");
  display.setTextSize(1);
  display.setCursor(10, 35);
  display.print("Starting...");
  display.display();

  // LEDs
  Serial.println("▶ LEDs...");
  leds.begin();
  leds.setBrightness(20);
  leds.clear();
  leds.show();

  // LED startup test (quick)
  for (int i = 0; i < NUM_LEDS; i++) {
    leds.setPixelColor(i, 0, 255, 0);  // Green
    leds.show();
    delay(100);
  }
  leds.clear();
  leds.show();
  Serial.println("✓ LEDs OK");

  // LoRa
  Serial.println("▶ LoRa...");
  initializeLoRa();

  // Load all settings
  Serial.println("▶ Loading settings...");
  loadWiFiCredentials();
  loadMqttSettings();
  loadLoRaSettings();
  loadAlertSettings();
  initializeTankSettings();

  Serial.printf("  WiFi: %s\n", savedSSID.length() > 0 ? savedSSID.c_str() : "None");
  Serial.printf("  LoRa Freq: %s, NetID: %d, Addr: %d\n", LORA_FREQUENCY.c_str(), LORA_NETWORK_ID, MY_ADDRESS);
  Serial.printf("  Capacity: %.1fL\n", tank.tankCapacity);
  Serial.printf("  Alerts: %s\n", alerts_enabled ? "Enabled" : "Disabled");

  // WiFi
  if (savedSSID.length() > 0) {
    startWiFiConnectionBlocking();
  } else {
    startAPMode();
  }

  // Web server
  Serial.println("▶ Web Server...");
  setupWebServer(webServer);

  // MQTT
  if (mqtt_enabled) {
    Serial.println("▶ MQTT...");
    initializeMqtt(mqtt);
  }

  // Initialize display timing
  lastScreenChange = millis();
  lastDisplayUpdate = millis();

  Serial.println("\n╔═══════════════════════════════════════════╗");
  Serial.println("║            RECEIVER READY                 ║");
  Serial.println("╚═══════════════════════════════════════════╝");

  Serial.println("\n📍 WS2812B LED Indicators (2 LEDs):");
  Serial.println("┌────────────────────────────────────────────┐");
  Serial.println("│ LED 0 - SYSTEM STATUS                     │");
  Serial.println("│   🔴 RED    = LoRa HW Missing / Error      │");
  Serial.println("│   🔵 BLUE   = AP Mode (needs WiFi setup)   │");
  Serial.println("│   🟡 YELLOW = Data Stale (>10 min)         │");
  Serial.println("│   🟢 GREEN  = All OK (WiFi + Fresh Data)   │");
  Serial.println("│   🔵 CYAN   = Waiting for First Data       │");
  Serial.println("├────────────────────────────────────────────┤");
  Serial.println("│ LED 1 - WATER LEVEL                        │");
  Serial.println("│   🟢 GREEN  = 75-100% Full                 │");
  Serial.println("│   🔵 CYAN   = 50-74% Good                  │");
  Serial.println("│   🟡 YELLOW = 25-49% Fair                  │");
  Serial.println("│   🟠 ORANGE = 10-24% Low (should refill)   │");
  Serial.println("│   🔴 RED    = 0-9% CRITICAL! (blink)       │");
  Serial.println("└────────────────────────────────────────────┘\n");
}

// ============================================================================
// MAIN LOOP - CRITICAL: yield() FREQUENTLY!
// ============================================================================
void loop() {
  delay(5);  // CRITICAL: Explicit hardware WDT feed at start of loop

  // Blink timer
  if (millis() - lastBlinkToggle > 500) {
    lastBlinkToggle = millis();
    blinkState = !blinkState;
  }
  delay(1);

  // WiFi connection check (non-blocking)
  updateWiFiConnection();
  delay(1);

  checkWiFiStatus();
  delay(1);

  // Web server
  webServer.handleClient();
  delay(5);  // Extra delay after web server (can be blocking)

  // MQTT
  if (wifiState == WIFI_STATE_CONNECTED && mqtt_enabled) {
    if (!mqtt.connected()) {
      delay(5);
      connectMqtt(mqtt);  // This publishes data on connect
      delay(5);
    } else {
      mqtt.loop();
      delay(1);
    }
    delay(1);
  }

  // LoRa processing
  processLoRaData();
  delay(5);  // Extra delay after LoRa processing

  // Publish MQTT only when NEW LoRa data arrives (not every 30 seconds)
  if (newDataReceived && mqtt_enabled && mqtt.connected()) {
    newDataReceived = false;  // Clear flag
    delay(5);
    publishMqttData(mqtt);
    delay(10);  // CRITICAL: Extra delay after MQTT publish to prevent crash
  }

  updateLoRaState();
  delay(1);

  // LEDs (update every 500ms)
  if (millis() - lastLedUpdate > 500) {
    lastLedUpdate = millis();
    updateLedColors();
    safeLedShow();
  }
  delay(1);

  // OLED Display
  updateDisplay(display);
  delay(5);  // Extra delay after display update

  delay(20);  // Base loop delay - feeds watchdog, ~40Hz loop rate
}

// ============================================================================
// LED FUNCTIONS
// ============================================================================
void updateLedColors() {
  // LED 0 - SYSTEM STATUS (WiFi + LoRa + Data Age)
  if (!loraHardwareConnected) {
    // Critical: LoRa hardware missing
    ledColors[LED_STATUS] = COLOR_RED;
  } else if (wifiState == WIFI_STATE_AP_MODE) {
    // AP Mode: Waiting for WiFi config
    ledColors[LED_STATUS] = blinkState ? COLOR_BLUE : COLOR_OFF;
  } else if (wifiState == WIFI_STATE_CONNECTED) {
    // WiFi connected - check LoRa data status
    if (loraState == STATE_CONNECTED) {
      // All good: WiFi OK, receiving fresh data
      ledColors[LED_STATUS] = COLOR_GREEN;
    } else if (loraState == STATE_STALE) {
      // Warning: Data is stale (>10 min old)
      ledColors[LED_STATUS] = COLOR_YELLOW;
    } else if (loraState == STATE_LOST) {
      // Warning: No data for >15 min
      ledColors[LED_STATUS] = blinkState ? COLOR_YELLOW : COLOR_OFF;
    } else {
      // Waiting for first data
      ledColors[LED_STATUS] = blinkState ? COLOR_CYAN : COLOR_DIM_CYAN;
    }
  } else {
    // Disconnected: trying to connect
    ledColors[LED_STATUS] = blinkState ? COLOR_ORANGE : COLOR_OFF;
  }

  // LED 1 - WATER LEVEL
  if (!tank.dataValid) {
    // No data yet: blink orange
    ledColors[LED_WATER] = blinkState ? COLOR_ORANGE : COLOR_OFF;
  } else if (tank.waterPercent >= 75) {
    // 75-100%: Full (green)
    ledColors[LED_WATER] = COLOR_GREEN;
  } else if (tank.waterPercent >= 50) {
    // 50-74%: Good (cyan)
    ledColors[LED_WATER] = COLOR_CYAN;
  } else if (tank.waterPercent >= 25) {
    // 25-49%: Fair (yellow)
    ledColors[LED_WATER] = COLOR_YELLOW;
  } else if (tank.waterPercent >= 10) {
    // 10-24%: Low (orange)
    ledColors[LED_WATER] = COLOR_ORANGE;
  } else {
    // 0-9%: CRITICAL! (blink red)
    ledColors[LED_WATER] = blinkState ? COLOR_RED : COLOR_OFF;
  }
}

void safeLedShow() {
  for (int i = 0; i < NUM_LEDS; i++) {
    leds.setPixelColor(i, ledColors[i].r, ledColors[i].g, ledColors[i].b);
  }
  leds.show();
}
