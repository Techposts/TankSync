/**
 * LoRa Water Tank Level Monitor - TRANSMITTER
 * 
 * Hardware:
 *   - ESP32-C3 SuperMini
 *   - RYLR998 LoRa Module (868/915 MHz)
 *   - JSN-SR04T Waterproof Ultrasonic Sensor
 *   - WS2812B LEDs (2x) for status indication
 *   - 3.7V Li-ion Battery with voltage divider
 * 
 * Features:
 *   - Deep sleep for battery conservation
 *   - WiFi/Bluetooth disabled for power saving
 *   - ACK-based reliable communication
 *   - Battery voltage monitoring
 *   - Visual status LEDs
 * 
 * Pin Assignments:
 *   - GPIO20: LoRa RX
 *   - GPIO21: LoRa TX
 *   - GPIO4:  Ultrasonic TRIG
 *   - GPIO5:  Ultrasonic ECHO
 *   - GPIO3:  Battery ADC (via voltage divider)
 *   - GPIO2:  WS2812B Data
 * 
 * Author: Your Name
 * License: MIT
 * Repository: https://github.com/yourusername/LoRa-Water-Tank-Monitor
 */

#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <Adafruit_NeoPixel.h>

// =============================================================================
// PIN DEFINITIONS
// =============================================================================
#define LORA_RX     20    // LoRa module TXD -> ESP32 RX
#define LORA_TX     21    // LoRa module RXD -> ESP32 TX
#define TRIG_PIN    4     // Ultrasonic trigger
#define ECHO_PIN    5     // Ultrasonic echo
#define BATTERY_PIN 3     // Battery voltage ADC (via divider)
#define LED_PIN     2     // WS2812B data pin
#define NUM_LEDS    2     // Number of status LEDs

// =============================================================================
// LED CONFIGURATION
// =============================================================================
#define LED_TX      0     // LED index for transmission status
#define LED_BATTERY 1     // LED index for battery status

// =============================================================================
// TIMING CONFIGURATION
// =============================================================================
#define SLEEP_MINUTES   5                               // Deep sleep duration
#define SLEEP_US        (SLEEP_MINUTES * 60 * 1000000ULL)  // Convert to microseconds
#define ACK_TIMEOUT     2000                            // ACK wait timeout (ms)
#define MAX_RETRIES     3                               // Max transmission attempts

// =============================================================================
// LORA CONFIGURATION
// =============================================================================
#define RECEIVER_ADDRESS  2           // Address of receiver module
#define MY_ADDRESS        1           // This transmitter's address
#define NETWORK_ID        6           // LoRa network ID (must match receiver)
#define FREQUENCY         "915000000" // RF frequency in Hz
                                      // Use "868000000" for EU
                                      // Use "915000000" for US/AU

// =============================================================================
// BATTERY CONFIGURATION
// =============================================================================
#define BATTERY_FULL_V   4.2    // Fully charged Li-ion voltage
#define BATTERY_EMPTY_V  3.0    // Empty Li-ion voltage (safe cutoff)
#define DIVIDER_RATIO    2.0    // Voltage divider ratio (100K + 100K)
#define ADC_MAX          4095   // ESP32 ADC maximum value (12-bit)
#define ADC_REF_V        3.3    // ADC reference voltage

// =============================================================================
// COLOR DEFINITIONS (GRB format for WS2812B)
// =============================================================================
const uint32_t COLOR_OFF     = 0x000000;
const uint32_t COLOR_RED     = 0xFF0000;
const uint32_t COLOR_GREEN   = 0x00FF00;
const uint32_t COLOR_BLUE    = 0x0000FF;
const uint32_t COLOR_YELLOW  = 0xFFFF00;
const uint32_t COLOR_CYAN    = 0x00FFFF;
const uint32_t COLOR_MAGENTA = 0xFF00FF;
const uint32_t COLOR_WHITE   = 0xFFFFFF;

// =============================================================================
// GLOBAL OBJECTS
// =============================================================================
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// =============================================================================
// FUNCTION PROTOTYPES
// =============================================================================
void sendCommand(String cmd);
int measureDistance();
float readBatteryVoltage();
int readBatteryPercent();
bool sendDataWithAck(int distance, int batteryPercent, float batteryVoltage);
bool waitForAck(unsigned long msgId);
void updateBatteryLed(int percent);
void setLed(int index, uint32_t color);
void flashLed(int index, uint32_t color, int times, int delayMs);

// =============================================================================
// SETUP - Runs once on wake
// =============================================================================
void setup() {
  // -------------------------------------------------------------------------
  // Disable WiFi and Bluetooth immediately for power saving
  // -------------------------------------------------------------------------
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  esp_bt_controller_disable();
  
  // -------------------------------------------------------------------------
  // Initialize Serial ports
  // -------------------------------------------------------------------------
  Serial.begin(115200);           // USB debug output
  Serial1.begin(115200, SERIAL_8N1, LORA_RX, LORA_TX);  // LoRa UART
  
  // -------------------------------------------------------------------------
  // Initialize NeoPixel LEDs
  // -------------------------------------------------------------------------
  strip.begin();
  strip.setBrightness(30);        // Low brightness for battery saving
  setLed(LED_TX, COLOR_YELLOW);   // Yellow = starting up
  setLed(LED_BATTERY, COLOR_OFF);
  strip.show();
  
  // -------------------------------------------------------------------------
  // Initialize sensor pins
  // -------------------------------------------------------------------------
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BATTERY_PIN, INPUT);
  
  // -------------------------------------------------------------------------
  // Wait for LoRa module to boot
  // -------------------------------------------------------------------------
  delay(1000);
  
  Serial.println("\n========================================");
  Serial.println("  LoRa Water Tank Monitor - Transmitter");
  Serial.println("========================================");
  
  // -------------------------------------------------------------------------
  // Configure LoRa module
  // -------------------------------------------------------------------------
  Serial.println("\nConfiguring LoRa module...");
  sendCommand("AT");
  sendCommand("AT+ADDRESS=" + String(MY_ADDRESS));
  sendCommand("AT+NETWORKID=" + String(NETWORK_ID));
  sendCommand("AT+BAND=" + String(FREQUENCY));
  sendCommand("AT+CRFOP=14");     // Medium TX power (14 dBm) for battery life
  Serial.println("LoRa configured.\n");
  
  // -------------------------------------------------------------------------
  // Take measurements
  // -------------------------------------------------------------------------
  Serial.println("Taking measurements...");
  int distance = measureDistance();
  float batteryVoltage = readBatteryVoltage();
  int batteryPercent = readBatteryPercent();
  
  Serial.println("  Distance: " + String(distance) + " cm");
  Serial.println("  Battery:  " + String(batteryPercent) + "% (" + 
                 String(batteryVoltage, 2) + "V)");
  
  // -------------------------------------------------------------------------
  // Update battery LED
  // -------------------------------------------------------------------------
  updateBatteryLed(batteryPercent);
  
  // -------------------------------------------------------------------------
  // Send data with ACK verification
  // -------------------------------------------------------------------------
  Serial.println("\nSending data to receiver...");
  bool success = sendDataWithAck(distance, batteryPercent, batteryVoltage);
  
  // -------------------------------------------------------------------------
  // Show result on LEDs
  // -------------------------------------------------------------------------
  if (success) {
    Serial.println("\n✓ Transmission successful!");
    flashLed(LED_TX, COLOR_GREEN, 3, 100);
  } else {
    Serial.println("\n✗ Transmission failed after " + String(MAX_RETRIES) + " attempts");
    flashLed(LED_TX, COLOR_RED, 5, 100);
  }
  
  // -------------------------------------------------------------------------
  // Turn off LEDs before sleep
  // -------------------------------------------------------------------------
  delay(500);
  setLed(LED_TX, COLOR_OFF);
  setLed(LED_BATTERY, COLOR_OFF);
  strip.show();
  
  // -------------------------------------------------------------------------
  // Put LoRa module to sleep
  // -------------------------------------------------------------------------
  sendCommand("AT+MODE=1");
  
  // -------------------------------------------------------------------------
  // Enter deep sleep
  // -------------------------------------------------------------------------
  Serial.println("\nEntering deep sleep for " + String(SLEEP_MINUTES) + " minutes...");
  Serial.println("========================================\n");
  Serial.flush();
  
  esp_sleep_enable_timer_wakeup(SLEEP_US);
  esp_deep_sleep_start();
}

// =============================================================================
// LOOP - Never runs (device sleeps after setup)
// =============================================================================
void loop() {
  // Not used - device enters deep sleep in setup()
}

// =============================================================================
// Send data with ACK verification and retries
// =============================================================================
bool sendDataWithAck(int distance, int batteryPercent, float batteryVoltage) {
  for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
    Serial.println("  Attempt " + String(attempt) + " of " + String(MAX_RETRIES));
    
    // Show blue LED while transmitting
    setLed(LED_TX, COLOR_BLUE);
    strip.show();
    
    // Generate unique message ID
    unsigned long msgId = millis() + (esp_random() % 1000);
    
    // Build payload: TANK:<distance>:<battery%>:<voltage>:<msgId>
    String payload = "TANK:" + String(distance) + ":" + 
                     String(batteryPercent) + ":" + 
                     String(batteryVoltage, 2) + ":" +
                     String(msgId);
    
    // Build AT command
    String cmd = "AT+SEND=" + String(RECEIVER_ADDRESS) + "," + 
                 String(payload.length()) + "," + payload;
    
    Serial.println("    Payload: " + payload);
    sendCommand(cmd);
    
    // Show cyan LED while waiting for ACK
    setLed(LED_TX, COLOR_CYAN);
    strip.show();
    
    // Wait for acknowledgment
    if (waitForAck(msgId)) {
      return true;
    }
    
    // Show red briefly on failure
    setLed(LED_TX, COLOR_RED);
    strip.show();
    delay(500);
  }
  
  return false;
}

// =============================================================================
// Wait for ACK response from receiver
// =============================================================================
bool waitForAck(unsigned long msgId) {
  unsigned long startTime = millis();
  String expectedAck = "ACK:" + String(msgId);
  
  Serial.println("    Waiting for ACK (timeout: " + String(ACK_TIMEOUT) + "ms)...");
  
  while (millis() - startTime < ACK_TIMEOUT) {
    while (Serial1.available()) {
      String response = Serial1.readStringUntil('\n');
      response.trim();
      
      Serial.println("    Received: " + response);
      
      // Check if this is our ACK
      if (response.startsWith("+RCV=")) {
        if (response.indexOf(expectedAck) > 0) {
          Serial.println("    ✓ ACK received!");
          return true;
        }
      }
    }
    delay(10);
  }
  
  Serial.println("    ✗ ACK timeout");
  return false;
}

// =============================================================================
// Measure distance using ultrasonic sensor
// =============================================================================
int measureDistance() {
  long totalDuration = 0;
  int validReadings = 0;
  
  // Take 5 readings and average them
  for (int i = 0; i < 5; i++) {
    // Send trigger pulse
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    
    // Measure echo duration (30ms timeout = ~5m max)
    long duration = pulseIn(ECHO_PIN, HIGH, 30000);
    
    if (duration > 0) {
      totalDuration += duration;
      validReadings++;
    }
    delay(50);
  }
  
  // Return error if no valid readings
  if (validReadings == 0) {
    Serial.println("  Warning: No valid ultrasonic readings!");
    return -1;
  }
  
  // Calculate average distance
  // Speed of sound = 343 m/s = 0.034 cm/µs
  // Distance = (duration * 0.034) / 2
  long avgDuration = totalDuration / validReadings;
  int distance = avgDuration * 0.034 / 2;
  
  return distance;
}

// =============================================================================
// Read battery voltage via ADC
// =============================================================================
float readBatteryVoltage() {
  long total = 0;
  
  // Take 10 readings and average
  for (int i = 0; i < 10; i++) {
    total += analogRead(BATTERY_PIN);
    delay(10);
  }
  
  float avgAdc = total / 10.0;
  
  // Convert ADC reading to voltage
  // Voltage = (ADC / ADC_MAX) * Reference * Divider_Ratio
  float voltage = (avgAdc / ADC_MAX) * ADC_REF_V * DIVIDER_RATIO;
  
  return voltage;
}

// =============================================================================
// Calculate battery percentage
// =============================================================================
int readBatteryPercent() {
  float voltage = readBatteryVoltage();
  
  // Linear mapping from voltage to percentage
  int percent = ((voltage - BATTERY_EMPTY_V) / (BATTERY_FULL_V - BATTERY_EMPTY_V)) * 100;
  
  return constrain(percent, 0, 100);
}

// =============================================================================
// Update battery status LED
// =============================================================================
void updateBatteryLed(int percent) {
  if (percent > 50) {
    setLed(LED_BATTERY, COLOR_GREEN);
  } else if (percent > 20) {
    setLed(LED_BATTERY, COLOR_YELLOW);
  } else {
    setLed(LED_BATTERY, COLOR_RED);
  }
  strip.show();
}

// =============================================================================
// Set single LED color
// =============================================================================
void setLed(int index, uint32_t color) {
  strip.setPixelColor(index, color);
}

// =============================================================================
// Flash LED multiple times
// =============================================================================
void flashLed(int index, uint32_t color, int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    setLed(index, color);
    strip.show();
    delay(delayMs);
    setLed(index, COLOR_OFF);
    strip.show();
    delay(delayMs);
  }
}

// =============================================================================
// Send AT command to LoRa module
// =============================================================================
void sendCommand(String cmd) {
  Serial1.println(cmd);
  delay(100);
  
  // Read and print response
  while (Serial1.available()) {
    String response = Serial1.readStringUntil('\n');
    Serial.println("  LoRa: " + response);
  }
}
