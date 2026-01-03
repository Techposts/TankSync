/**
 * ============================================================================
 * LoRa Water Tank Level Monitor - TRANSMITTER
 * ============================================================================
 * 
 * Hardware Configuration:
 *   - ESP32-C3 SuperMini
 *   - RYLR998 LoRa Module (868/915 MHz)
 *   - AJ-SR04M Waterproof Ultrasonic Sensor
 *   - WS2812B LED (1x) - GPIO powered for zero sleep current
 *   - 18650 Battery with 10KΩ + 10KΩ voltage divider
 *   - TP4056 Solar charging module
 * 
 * Pin Connections:
 *   ┌─────────────┬─────────────┬─────────────────────────────┐
 *   │ ESP32-C3    │ Component   │ Notes                       │
 *   ├─────────────┼─────────────┼─────────────────────────────┤
 *   │ GPIO21      │ RYLR998 RXD │ ESP TX → LoRa RX            │
 *   │ GPIO20      │ RYLR998 TXD │ ESP RX ← LoRa TX            │
 *   │ 3.3V        │ RYLR998 VDD │ Power                       │
 *   │ GND         │ RYLR998 GND │ Ground                      │
 *   │ GPIO4       │ AJ-SR04M TRIG │ Trigger                  │
 *   │ GPIO5       │ AJ-SR04M ECHO │ Echo                     │
 *   │ 3.3V        │ AJ-SR04M VCC  │ Power                    │
 *   │ GND         │ AJ-SR04M GND  │ Ground                   │
 *   │ GPIO3       │ Voltage Divider │ ADC (mid-point)         │
 *   │ GPIO7       │ WS2812B VCC  │ Power control              │
 *   │ GPIO2       │ WS2812B DIN  │ Data                       │
 *   │ GND         │ WS2812B GND  │ Ground                     │
 *   │ 5V          │ Battery+     │ Via TP4056                 │
 *   └─────────────┴─────────────┴─────────────────────────────┘
 * 
 * Voltage Divider (10KΩ + 10KΩ):
 *   Battery+ ── 10KΩ ──┬── 10KΩ ── GND
 *                      │
 *                    GPIO3
 * 
 * Author: Ravi
 * License: MIT
 * ============================================================================
 */

#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <Adafruit_NeoPixel.h>

// ============================================================================
// PIN DEFINITIONS
// ============================================================================
#define LORA_RX_PIN     20    // ESP32 RX ← RYLR998 TXD (Pin 4)
#define LORA_TX_PIN     21    // ESP32 TX → RYLR998 RXD (Pin 3)
#define TRIG_PIN        4     // AJ-SR04M Trigger
#define ECHO_PIN        5     // AJ-SR04M Echo
#define BATTERY_ADC_PIN 3     // Voltage divider mid-point
#define LED_DATA_PIN    2     // WS2812B Data
#define LED_POWER_PIN   7     // WS2812B Power (GPIO driven)

// ============================================================================
// CONFIGURATION - ADJUST THESE VALUES
// ============================================================================

// Sleep Configuration
#define SLEEP_MINUTES       5                                   // Deep sleep duration
#define SLEEP_MICROSECONDS  (SLEEP_MINUTES * 60 * 1000000ULL)   // Convert to µs

// LoRa Configuration
#define LORA_FREQUENCY      "865000000"   // 865 MHz for India
                                          // Use "868000000" for EU
                                          // Use "915000000" for US/AU
#define LORA_NETWORK_ID     6             // Must match receiver (3-15 or 18)
#define LORA_TX_POWER       14            // TX power in dBm (0-22)
#define MY_ADDRESS          1             // This transmitter's address
#define RECEIVER_ADDRESS    2             // Receiver's address

// Communication Settings
#define ACK_TIMEOUT_MS      2000          // Wait time for ACK (ms)
#define MAX_RETRIES         3             // Transmission retry attempts
#define LORA_BAUD_RATE      115200        // RYLR998 default baud rate

// Battery Configuration (10KΩ + 10KΩ voltage divider)
#define VOLTAGE_DIVIDER_RATIO   2.0       // (R1 + R2) / R2 = (10K + 10K) / 10K
#define ADC_RESOLUTION          4095.0    // 12-bit ADC
#define ADC_REFERENCE_VOLTAGE   3.3       // ESP32 ADC reference
#define BATTERY_FULL_VOLTAGE    4.2       // Fully charged Li-ion
#define BATTERY_EMPTY_VOLTAGE   3.0       // Safe discharge cutoff

// AJ-SR04M Sensor Configuration (optimized for stable readings)
#define SENSOR_NUM_READINGS     7         // Take 7 readings (will use median)
#define SENSOR_READING_INTERVAL 120       // 120ms between readings (AJ-SR04M minimum ~100ms)
#define SENSOR_TRIGGER_PULSE_US 10        // 10µs trigger pulse
#define SENSOR_ECHO_TIMEOUT_US  38000     // 38ms timeout (~6.5m max range)
#define SENSOR_SETTLE_TIME_US   5         // 5µs settle time before trigger

// Calibration (adjust if readings are off)
#define ADC_CALIBRATION_FACTOR  0.918       // Fine-tune ADC reading (0.95-1.05)
#define DISTANCE_OFFSET_CM      0         // Add/subtract from distance reading

// ============================================================================
// LED COLORS (GRB format for WS2812B)
// ============================================================================
#define COLOR_OFF       0x000000
#define COLOR_RED       0xFF0000
#define COLOR_GREEN     0x00FF00
#define COLOR_BLUE      0x0000FF
#define COLOR_YELLOW    0xFFFF00
#define COLOR_CYAN      0x00FFFF
#define COLOR_MAGENTA   0xFF00FF
#define COLOR_ORANGE    0xFFA500
#define COLOR_WHITE     0xFFFFFF

// ============================================================================
// GLOBAL OBJECTS
// ============================================================================
Adafruit_NeoPixel led(1, LED_DATA_PIN, NEO_GRB + NEO_KHZ800);

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
unsigned long messageId = 0;
bool loraReady = false;

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================
void disableRadios();
void initializeLed();
void powerOnLed();
void powerOffLed();
void setLedColor(uint32_t color);
void blinkLed(uint32_t color, int times, int delayMs);
void initializeLoRa();
bool sendLoRaCommand(String command, String expectedResponse = "+OK", int timeout = 1000);
String getLoRaResponse(int timeout);
int measureDistance();
int getMedianDistance(int* readings, int count);
float readBatteryVoltage();
int calculateBatteryPercent(float voltage);
bool transmitData(int distance, int batteryPercent, float batteryVoltage);
bool waitForAck(unsigned long msgId);
void enterDeepSleep();

// ============================================================================
// SETUP - Runs once on each wake
// ============================================================================
void setup() {
  // Disable WiFi and Bluetooth immediately for power saving
  disableRadios();
  
  // Initialize serial for debugging
  Serial.begin(115200);
  delay(100);
  
  Serial.println();
  Serial.println("╔════════════════════════════════════════════════════════════╗");
  Serial.println("║       LoRa Water Tank Monitor - TRANSMITTER                ║");
  Serial.println("╠════════════════════════════════════════════════════════════╣");
  Serial.printf ("║  Frequency: %s Hz                              ║\n", LORA_FREQUENCY);
  Serial.printf ("║  Network ID: %d    TX Power: %d dBm                        ║\n", LORA_NETWORK_ID, LORA_TX_POWER);
  Serial.printf ("║  My Address: %d    Receiver: %d                            ║\n", MY_ADDRESS, RECEIVER_ADDRESS);
  Serial.printf ("║  Sleep Time: %d minutes                                    ║\n", SLEEP_MINUTES);
  Serial.println("╚════════════════════════════════════════════════════════════╝");
  Serial.println();
  
  // Initialize LED
  initializeLed();
  powerOnLed();
  setLedColor(COLOR_YELLOW);  // Yellow = Starting up
  
  // Initialize sensor pins
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BATTERY_ADC_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);
  
  // Initialize LoRa
  Serial.println("▶ Initializing LoRa module...");
  initializeLoRa();
  
  if (!loraReady) {
    Serial.println("✗ LoRa initialization failed!");
    blinkLed(COLOR_RED, 10, 100);
    enterDeepSleep();
    return;
  }
  
  Serial.println("✓ LoRa ready");
  setLedColor(COLOR_BLUE);  // Blue = Taking measurements
  
  // Take measurements
  Serial.println();
  Serial.println("▶ Taking measurements...");

  delay(200);  // Let AJ-SR04M sensor stabilize

  int distance = measureDistance();
  float batteryVoltage = readBatteryVoltage();
  int batteryPercent = calculateBatteryPercent(batteryVoltage);
  
  Serial.println("┌────────────────────────────────────┐");
  Serial.printf ("│  Distance:     %4d cm             │\n", distance);
  Serial.printf ("│  Battery:      %3d%% (%.2fV)       │\n", batteryPercent, batteryVoltage);
  Serial.println("└────────────────────────────────────┘");
  
  // Transmit data
  Serial.println();
  Serial.println("▶ Transmitting data...");
  setLedColor(COLOR_CYAN);  // Cyan = Transmitting
  
  bool success = transmitData(distance, batteryPercent, batteryVoltage);
  
  // Show result
  if (success) {
    Serial.println();
    Serial.println("╔════════════════════════════════════╗");
    Serial.println("║     ✓ TRANSMISSION SUCCESSFUL      ║");
    Serial.println("╚════════════════════════════════════╝");
    blinkLed(COLOR_GREEN, 3, 150);
  } else {
    Serial.println();
    Serial.println("╔════════════════════════════════════╗");
    Serial.println("║     ✗ TRANSMISSION FAILED          ║");
    Serial.println("╚════════════════════════════════════╝");
    blinkLed(COLOR_RED, 5, 100);
  }
  
  // Enter deep sleep
  enterDeepSleep();
}

// ============================================================================
// LOOP - Never executes (device sleeps after setup)
// ============================================================================
void loop() {
  // Not used - device enters deep sleep in setup()
}

// ============================================================================
// DISABLE WIFI AND BLUETOOTH
// ============================================================================
void disableRadios() {
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  esp_bt_controller_disable();
}

// ============================================================================
// LED FUNCTIONS
// ============================================================================
void initializeLed() {
  pinMode(LED_POWER_PIN, OUTPUT);
  digitalWrite(LED_POWER_PIN, LOW);
}

void powerOnLed() {
  digitalWrite(LED_POWER_PIN, HIGH);
  delay(5);  // Let power stabilize
  led.begin();
  led.setBrightness(50);
  led.show();
}

void powerOffLed() {
  led.setPixelColor(0, COLOR_OFF);
  led.show();
  delay(5);
  digitalWrite(LED_POWER_PIN, LOW);
  pinMode(LED_POWER_PIN, INPUT);  // High-Z for minimum leakage
}

void setLedColor(uint32_t color) {
  led.setPixelColor(0, color);
  led.show();
}

void blinkLed(uint32_t color, int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    setLedColor(color);
    delay(delayMs);
    setLedColor(COLOR_OFF);
    delay(delayMs);
  }
}

// ============================================================================
// LORA INITIALIZATION
// ============================================================================
void initializeLoRa() {
  // Initialize UART for LoRa module
  Serial1.begin(LORA_BAUD_RATE, SERIAL_8N1, LORA_RX_PIN, LORA_TX_PIN);
  delay(500);  // Wait for module to boot
  
  // Clear any pending data
  while (Serial1.available()) {
    Serial1.read();
  }
  
  // Test communication
  if (!sendLoRaCommand("AT", "+OK")) {
    Serial.println("  ✗ LoRa not responding");
    loraReady = false;
    return;
  }
  Serial.println("  ✓ LoRa communication OK");
  
  // Configure address
  if (!sendLoRaCommand("AT+ADDRESS=" + String(MY_ADDRESS))) {
    Serial.println("  ✗ Failed to set address");
  } else {
    Serial.printf("  ✓ Address set to %d\n", MY_ADDRESS);
  }
  
  // Configure network ID
  if (!sendLoRaCommand("AT+NETWORKID=" + String(LORA_NETWORK_ID))) {
    Serial.println("  ✗ Failed to set network ID");
  } else {
    Serial.printf("  ✓ Network ID set to %d\n", LORA_NETWORK_ID);
  }
  
  // Configure frequency
  if (!sendLoRaCommand("AT+BAND=" + String(LORA_FREQUENCY))) {
    Serial.println("  ✗ Failed to set frequency");
  } else {
    Serial.printf("  ✓ Frequency set to %s Hz\n", LORA_FREQUENCY);
  }
  
  // Configure TX power
  if (!sendLoRaCommand("AT+CRFOP=" + String(LORA_TX_POWER))) {
    Serial.println("  ✗ Failed to set TX power");
  } else {
    Serial.printf("  ✓ TX power set to %d dBm\n", LORA_TX_POWER);
  }
  
  loraReady = true;
}

// ============================================================================
// SEND LORA AT COMMAND
// ============================================================================
bool sendLoRaCommand(String command, String expectedResponse, int timeout) {
  // Clear buffer
  while (Serial1.available()) {
    Serial1.read();
  }
  
  // Send command
  Serial1.println(command);
  Serial.print("    TX: ");
  Serial.println(command);
  
  // Wait for response
  String response = getLoRaResponse(timeout);
  Serial.print("    RX: ");
  Serial.println(response);
  
  return response.indexOf(expectedResponse) >= 0;
}

// ============================================================================
// GET LORA RESPONSE
// ============================================================================
String getLoRaResponse(int timeout) {
  String response = "";
  unsigned long startTime = millis();
  
  while (millis() - startTime < timeout) {
    while (Serial1.available()) {
      char c = Serial1.read();
      response += c;
      if (c == '\n') {
        response.trim();
        return response;
      }
    }
    delay(10);
  }
  
  response.trim();
  return response;
}

// ============================================================================
// MEASURE DISTANCE (AJ-SR04M Optimized)
// ============================================================================
int measureDistance() {
  int readings[SENSOR_NUM_READINGS];
  int validReadings = 0;

  Serial.println("  AJ-SR04M Sensor Readings:");
  Serial.println("  ┌──────┬──────────┬──────────┐");
  Serial.println("  │  #   │ Duration │ Distance │");
  Serial.println("  ├──────┼──────────┼──────────┤");

  for (int i = 0; i < SENSOR_NUM_READINGS; i++) {
    // CRITICAL: Ensure trigger pin is LOW and stable
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(SENSOR_SETTLE_TIME_US);

    // Send trigger pulse (AJ-SR04M needs clean 10µs pulse)
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(SENSOR_TRIGGER_PULSE_US);
    digitalWrite(TRIG_PIN, LOW);

    // Measure echo duration
    // AJ-SR04M max range ~6.5m = ~38ms round trip
    long duration = pulseIn(ECHO_PIN, HIGH, SENSOR_ECHO_TIMEOUT_US);

    if (duration > 0 && duration < SENSOR_ECHO_TIMEOUT_US) {
      // Calculate distance
      // Speed of sound = 343 m/s = 0.0343 cm/µs
      // Distance = (duration × 0.0343) / 2
      int distance = (duration * 0.0343) / 2;

      // Store valid reading
      readings[validReadings] = distance;
      validReadings++;

      Serial.printf("  │  %d   │ %6ld µs │  %4d cm │\n", i + 1, duration, distance);
    } else {
      Serial.printf("  │  %d   │  TIMEOUT │   ---    │\n", i + 1);
    }

    // CRITICAL: AJ-SR04M needs minimum 100ms between measurements
    delay(SENSOR_READING_INTERVAL);
  }

  Serial.println("  └──────┴──────────┴──────────┘");

  // Check for valid readings
  if (validReadings == 0) {
    Serial.println("  ✗ No valid ultrasonic readings!");
    Serial.println("    Check sensor connections and power");
    return -1;
  }

  if (validReadings < 3) {
    Serial.printf("  ⚠ Only %d valid readings (unstable)\n", validReadings);
  }

  // Use median instead of average for better noise rejection
  int medianDistance = getMedianDistance(readings, validReadings);

  // Apply calibration offset
  medianDistance += DISTANCE_OFFSET_CM;

  Serial.printf("  📊 Median: %d cm (%d valid readings)\n", medianDistance, validReadings);

  return medianDistance;
}

// ============================================================================
// GET MEDIAN DISTANCE (better than average for noisy data)
// ============================================================================
int getMedianDistance(int* readings, int count) {
  // Simple bubble sort
  for (int i = 0; i < count - 1; i++) {
    for (int j = 0; j < count - i - 1; j++) {
      if (readings[j] > readings[j + 1]) {
        int temp = readings[j];
        readings[j] = readings[j + 1];
        readings[j + 1] = temp;
      }
    }
  }

  // Return median
  if (count % 2 == 0) {
    return (readings[count / 2 - 1] + readings[count / 2]) / 2;
  } else {
    return readings[count / 2];
  }
}

// ============================================================================
// READ BATTERY VOLTAGE
// ============================================================================
float readBatteryVoltage() {
  const int NUM_READINGS = 10;
  const int READING_DELAY = 10;  // ms between readings
  
  long totalAdc = 0;
  
  for (int i = 0; i < NUM_READINGS; i++) {
    totalAdc += analogRead(BATTERY_ADC_PIN);
    delay(READING_DELAY);
  }
  
  float avgAdc = totalAdc / (float)NUM_READINGS;
  
  // Convert ADC to voltage
  // V_adc = (ADC_value / ADC_resolution) × V_reference
  // V_battery = V_adc × Divider_ratio
  float adcVoltage = (avgAdc / ADC_RESOLUTION) * ADC_REFERENCE_VOLTAGE;
  float batteryVoltage = adcVoltage * VOLTAGE_DIVIDER_RATIO * ADC_CALIBRATION_FACTOR;
  
  Serial.printf("  ADC: %.1f → %.2fV (divider) → %.2fV (battery)\n", 
                avgAdc, adcVoltage, batteryVoltage);
  
  return batteryVoltage;
}

// ============================================================================
// CALCULATE BATTERY PERCENTAGE
// ============================================================================
int calculateBatteryPercent(float voltage) {
  // Linear mapping from voltage to percentage
  // This is simplified - Li-ion discharge is not perfectly linear
  float percent = ((voltage - BATTERY_EMPTY_VOLTAGE) / 
                   (BATTERY_FULL_VOLTAGE - BATTERY_EMPTY_VOLTAGE)) * 100.0;
  
  return constrain((int)percent, 0, 100);
}

// ============================================================================
// TRANSMIT DATA WITH ACK
// ============================================================================
bool transmitData(int distance, int batteryPercent, float batteryVoltage) {
  for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
    Serial.printf("  Attempt %d of %d...\n", attempt, MAX_RETRIES);
    
    // Generate unique message ID
    messageId = millis() + (esp_random() % 10000);
    
    // Build payload: TANK:<distance>:<battery%>:<voltage>:<msgId>
    String payload = "TANK:" + String(distance) + ":" + 
                     String(batteryPercent) + ":" + 
                     String(batteryVoltage, 2) + ":" +
                     String(messageId);
    
    // Build AT+SEND command
    String command = "AT+SEND=" + String(RECEIVER_ADDRESS) + "," + 
                     String(payload.length()) + "," + payload;
    
    Serial.println("    Payload: " + payload);
    
    // Send data
    Serial1.println(command);
    delay(100);
    
    // Read send response
    String sendResponse = getLoRaResponse(1000);
    Serial.println("    Response: " + sendResponse);
    
    if (sendResponse.indexOf("+OK") < 0) {
      Serial.println("    ✗ Send command failed");
      continue;
    }
    
    // Wait for ACK
    setLedColor(COLOR_MAGENTA);  // Magenta = Waiting for ACK
    
    if (waitForAck(messageId)) {
      return true;
    }
    
    Serial.println("    ✗ No ACK received");
    setLedColor(COLOR_ORANGE);  // Orange = Retry
    delay(500);
  }
  
  return false;
}

// ============================================================================
// WAIT FOR ACK FROM RECEIVER
// ============================================================================
bool waitForAck(unsigned long msgId) {
  String expectedAck = "ACK:" + String(msgId);
  unsigned long startTime = millis();
  
  Serial.printf("    Waiting for ACK (timeout: %d ms)...\n", ACK_TIMEOUT_MS);
  
  while (millis() - startTime < ACK_TIMEOUT_MS) {
    if (Serial1.available()) {
      String response = Serial1.readStringUntil('\n');
      response.trim();
      
      Serial.println("    Received: " + response);
      
      // Check for +RCV message containing our ACK
      if (response.startsWith("+RCV=")) {
        if (response.indexOf(expectedAck) > 0) {
          Serial.println("    ✓ ACK verified!");
          return true;
        }
      }
    }
    delay(10);
  }
  
  return false;
}

// ============================================================================
// ENTER DEEP SLEEP
// ============================================================================
void enterDeepSleep() {
  Serial.println();
  Serial.printf("▶ Entering deep sleep for %d minutes...\n", SLEEP_MINUTES);
  Serial.println("════════════════════════════════════════════════════════════");
  Serial.println();
  Serial.flush();
  
  // Turn off LED before sleep
  powerOffLed();
  
  // Put LoRa module to sleep
  sendLoRaCommand("AT+MODE=1", "+OK", 500);
  
  // Configure and enter deep sleep
  esp_sleep_enable_timer_wakeup(SLEEP_MICROSECONDS);
  esp_deep_sleep_start();
}
