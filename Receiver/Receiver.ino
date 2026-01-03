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

#if DEBUG_VERBOSE
  Serial.println("\n=== TankSync Receiver - ESP32-C3 ===");
  Serial.println("WDT: Arduino framework (yield every ~20ms)");
#else
  Serial.println("\nTankSync v1.0");
#endif

  // Initialize preferences
  preferences.begin("tanksync", false);

  // I2C and OLED
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
  display.print("Initializing");
  display.display();

  // LEDs - Startup indicator
  leds.begin();
  leds.setBrightness(75);  // Increased from 20 for better visibility
  leds.clear();
  leds.show();

  // LED startup: Both LEDs pulse CYAN to show boot
  for (int brightness = 0; brightness < 100; brightness += 20) {
    leds.setPixelColor(0, 0, brightness, brightness);  // Cyan
    leds.setPixelColor(1, 0, brightness, brightness);  // Cyan
    leds.show();
    delay(50);
  }
  delay(200);
  leds.clear();
  leds.show();

  // LoRa init - LED 1 shows ORANGE during LoRa init
  leds.setPixelColor(LED_STATUS, 255, 128, 0);  // Orange = initializing
  leds.show();

  initializeLoRa();

  // LoRa result indicator
  if (loraHardwareConnected) {
    leds.setPixelColor(LED_STATUS, 0, 255, 0);  // Green = LoRa OK
  } else {
    leds.setPixelColor(LED_STATUS, 255, 0, 0);  // Red = LoRa failed
  }
  leds.show();
  delay(500);

  // Load all settings
  loadWiFiCredentials();
  loadMqttSettings();
  loadLoRaSettings();
  loadAlertSettings();
  initializeTankSettings();

#if DEBUG_VERBOSE
  Serial.printf("WiFi: %s | LoRa: %s MHz, Net:%d, Addr:%d\n",
                savedSSID.length() > 0 ? savedSSID.c_str() : "None",
                LORA_FREQUENCY.c_str(), LORA_NETWORK_ID, MY_ADDRESS);
  Serial.printf("Tank: %.1fL | Alerts: %s\n",
                tank.tankCapacity, alerts_enabled ? "On" : "Off");
#endif

  // WiFi - Update display to show connection attempt
  if (savedSSID.length() > 0) {
    // Show WiFi connecting on display
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("WiFi:");
    display.setTextSize(2);
    display.setCursor(0, 15);

    // Truncate SSID if too long for display (16 chars max for size 2)
    String displaySSID = savedSSID;
    if (displaySSID.length() > 10) {
      displaySSID = displaySSID.substring(0, 10);
    }
    display.print(displaySSID);

    display.setTextSize(1);
    display.setCursor(0, 40);
    display.print("Connecting...");
    display.display();

    // LED 1 blinks BLUE during WiFi connection
    startWiFiConnectionBlocking(display, leds);
  } else {
    // No WiFi configured - show AP mode on display
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("No WiFi Saved");
    display.setTextSize(2);
    display.setCursor(0, 20);
    display.print("AP Mode");
    display.setTextSize(1);
    display.setCursor(0, 45);
    display.print("192.168.4.1");
    display.display();

    startAPMode();
  }

  // Web server
  setupWebServer(webServer);

  // MQTT
  if (mqtt_enabled) {
    initializeMqtt(mqtt);
  }

  // Show READY screen
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(5, 10);
  display.print("READY!");
  display.setTextSize(1);
  display.setCursor(0, 35);
  if (wifiState == WIFI_STATE_CONNECTED) {
    display.print("WiFi: ");
    String ssidShort = savedSSID;
    if (ssidShort.length() > 10) ssidShort = ssidShort.substring(0, 10);
    display.print(ssidShort);
  } else {
    display.print("AP: 192.168.4.1");
  }
  display.setCursor(0, 50);
  display.print("LoRa: ");
  display.print(loraHardwareConnected ? "OK" : "ERROR");
  display.display();
  delay(2000);  // Show ready screen for 2 seconds

  // Clear LEDs before entering main loop (main loop will control them)
  leds.clear();
  leds.show();

  // Initialize display timing
  lastScreenChange = millis();
  lastDisplayUpdate = millis();

  Serial.println("READY | LEDs: 1=Status, 0=Water");

#if DEBUG_VERBOSE
  Serial.println("\nLED Guide:");
  Serial.println("  LED 1 (Status): RED=LoRa Error, BLUE=AP Mode, YELLOW=Stale, GREEN=OK");
  Serial.println("  LED 0 (Water): GREEN=Full, CYAN=Good, YELLOW=Fair, ORANGE=Low, RED=Critical");
#endif
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
  // LED 1 - SYSTEM STATUS (WiFi + LoRa + Data Age)
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

  // LED 0 - WATER LEVEL
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
