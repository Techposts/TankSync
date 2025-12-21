/**
 * LoRa Water Tank Level Monitor - RECEIVER
 * 
 * Hardware:
 *   - ESP32-C3 SuperMini
 *   - RYLR998 LoRa Module (868/915 MHz)
 *   - WS2812B LEDs (3x) for status indication
 * 
 * Features:
 *   - WiFi connected for Home Assistant integration
 *   - MQTT publishing for sensor data
 *   - Real-time web dashboard with auto-refresh
 *   - ACK-based reliable communication
 *   - Visual status LEDs for connection, water level, signal
 * 
 * Pin Assignments:
 *   - GPIO20: LoRa RX
 *   - GPIO21: LoRa TX
 *   - GPIO2:  WS2812B Data
 * 
 * Author: Ravi Singh (TechPosts Guides)
 * License: MIT
 * Repository: https://github.com/techposts/LoRa-Water-Tank-Monitor
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <Adafruit_NeoPixel.h>

// =============================================================================
// PIN DEFINITIONS
// =============================================================================
#define LORA_RX   20    // LoRa module TXD -> ESP32 RX
#define LORA_TX   21    // LoRa module RXD -> ESP32 TX
#define LED_PIN   2     // WS2812B data pin
#define NUM_LEDS  3     // Number of status LEDs

// =============================================================================
// LED CONFIGURATION
// =============================================================================
#define LED_STATUS  0   // Connection status LED
#define LED_WATER   1   // Water level LED
#define LED_SIGNAL  2   // Signal strength LED

// =============================================================================
// WIFI CONFIGURATION
// =============================================================================
const char* ssid = "YOUR_WIFI_SSID";          // Your WiFi network name
const char* password = "YOUR_WIFI_PASSWORD";   // Your WiFi password

// =============================================================================
// MQTT CONFIGURATION
// =============================================================================
const char* mqtt_server = "192.168.0.163";    // MQTT broker IP
const int mqtt_port = 1885;                    // MQTT broker port
const char* mqtt_user = "mqtt-user";           // MQTT username
const char* mqtt_password = "techposts";       // MQTT password
const char* mqtt_client_id = "TankReceiver";   // MQTT client ID

// =============================================================================
// TANK CONFIGURATION
// =============================================================================
const int TANK_DEPTH = 120;       // Tank depth in cm
const int SENSOR_OFFSET = 25;     // Distance from sensor to full water level

// =============================================================================
// LORA CONFIGURATION
// =============================================================================
#define MY_ADDRESS          2             // This receiver's address
#define TRANSMITTER_ADDRESS 1             // Expected transmitter address
#define NETWORK_ID          6             // LoRa network ID (must match TX)
#define FREQUENCY           "915000000"   // RF frequency (must match TX)

// =============================================================================
// TIMEOUT CONFIGURATION
// =============================================================================
#define DATA_TIMEOUT_MS       600000    // 10 minutes - data considered stale
#define CONNECTION_TIMEOUT_MS 900000    // 15 minutes - connection lost

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
const uint32_t COLOR_ORANGE  = 0xFF8000;
const uint32_t COLOR_WHITE   = 0xFFFFFF;

// =============================================================================
// GLOBAL OBJECTS
// =============================================================================
WiFiClient espClient;
PubSubClient mqtt(espClient);
WebServer server(80);
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// =============================================================================
// TANK DATA STRUCTURE
// =============================================================================
struct TankData {
  int rawDistance = 0;          // Raw sensor reading (cm)
  int waterLevel = 0;           // Calculated water height (cm)
  int waterPercent = 0;         // Water level percentage
  int batteryPercent = 0;       // Transmitter battery %
  float batteryVoltage = 0.0;   // Transmitter battery voltage
  int rssi = 0;                 // Signal strength (dBm)
  int snr = 0;                  // Signal-to-noise ratio
  unsigned long lastUpdate = 0; // Last data received timestamp
  unsigned long msgId = 0;      // Last message ID
  bool dataValid = false;       // Data validity flag
  int packetsReceived = 0;      // Total packets received
};

TankData tank;

// =============================================================================
// CONNECTION STATE ENUM
// =============================================================================
enum ConnectionState {
  STATE_STARTING,   // Initial boot
  STATE_WAITING,    // Waiting for first data
  STATE_CONNECTED,  // Receiving data normally
  STATE_STALE,      // Data is old (>10 min)
  STATE_LOST        // No data for extended period (>15 min)
};

ConnectionState connState = STATE_STARTING;

// =============================================================================
// TIMING VARIABLES
// =============================================================================
unsigned long lastMqttReconnect = 0;
unsigned long lastLedUpdate = 0;
unsigned long lastStatusBlink = 0;
bool statusBlinkOn = false;

// =============================================================================
// FUNCTION PROTOTYPES
// =============================================================================
void connectWiFi();
void reconnectMqtt();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void parseLoraMessage(String rcv);
void sendAck(int toAddress, unsigned long msgId);
void calculateWaterLevel();
void publishMqtt();
void updateConnectionState();
void updateAllLeds();
void updateStatusLed();
void updateWaterLed();
void updateSignalLed();
void setLed(int index, uint32_t color);
void setAllLeds(uint32_t color);
void flashLed(int index, uint32_t color, int times, int delayMs);
void setupWebServer();
void handleRoot();
void handleData();
void sendLoraCommand(String cmd);

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, LORA_RX, LORA_TX);
  
  // Initialize LEDs
  strip.begin();
  strip.setBrightness(50);
  setAllLeds(COLOR_YELLOW);
  
  delay(1000);
  
  Serial.println("\n========================================");
  Serial.println("  LoRa Water Tank Monitor - Receiver");
  Serial.println("========================================");
  
  // Configure LoRa module
  Serial.println("\nConfiguring LoRa module...");
  sendLoraCommand("AT");
  sendLoraCommand("AT+ADDRESS=" + String(MY_ADDRESS));
  sendLoraCommand("AT+NETWORKID=" + String(NETWORK_ID));
  sendLoraCommand("AT+BAND=" + String(FREQUENCY));
  Serial.println("LoRa configured.");
  
  // Connect to WiFi
  connectWiFi();
  
  // Setup MQTT
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(mqttCallback);
  
  // Setup Web Server
  setupWebServer();
  
  // Ready - waiting for data
  connState = STATE_WAITING;
  updateAllLeds();
  
  Serial.println("\n========================================");
  Serial.println("  RECEIVER READY");
  Serial.println("  IP Address: " + WiFi.localIP().toString());
  Serial.println("  Web UI: http://" + WiFi.localIP().toString());
  Serial.println("========================================\n");
}

// =============================================================================
// MAIN LOOP
// =============================================================================
void loop() {
  // Handle WiFi reconnection
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  
  // Handle MQTT connection
  if (!mqtt.connected()) {
    reconnectMqtt();
  }
  mqtt.loop();
  
  // Handle web server requests
  server.handleClient();
  
  // Check for incoming LoRa messages
  while (Serial1.available()) {
    String response = Serial1.readStringUntil('\n');
    response.trim();
    
    if (response.startsWith("+RCV=")) {
      parseLoraMessage(response);
    }
  }
  
  // Update connection state
  updateConnectionState();
  
  // Update LEDs periodically
  if (millis() - lastLedUpdate > 100) {
    lastLedUpdate = millis();
    updateAllLeds();
  }
}

// =============================================================================
// WIFI CONNECTION
// =============================================================================
void connectWiFi() {
  Serial.print("Connecting to WiFi");
  setLed(LED_STATUS, COLOR_YELLOW);
  strip.show();
  
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi connected: " + WiFi.localIP().toString());
    flashLed(LED_STATUS, COLOR_GREEN, 3, 100);
  } else {
    Serial.println("\n✗ WiFi connection failed!");
    flashLed(LED_STATUS, COLOR_RED, 5, 100);
  }
}

// =============================================================================
// MQTT RECONNECTION
// =============================================================================
void reconnectMqtt() {
  if (millis() - lastMqttReconnect < 5000) return;
  lastMqttReconnect = millis();
  
  Serial.print("Connecting to MQTT...");
  
  if (mqtt.connect(mqtt_client_id, mqtt_user, mqtt_password)) {
    Serial.println(" connected");
    mqtt.subscribe("tank/command");
  } else {
    Serial.println(" failed (rc=" + String(mqtt.state()) + ")");
  }
}

// =============================================================================
// MQTT CALLBACK
// =============================================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Handle incoming MQTT commands if needed
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println("MQTT received [" + String(topic) + "]: " + message);
}

// =============================================================================
// PARSE LORA MESSAGE
// =============================================================================
void parseLoraMessage(String rcv) {
  Serial.println("LoRa received: " + rcv);
  
  // Flash status LED white on receive
  setLed(LED_STATUS, COLOR_WHITE);
  strip.show();
  
  // Parse format: +RCV=<addr>,<len>,<data>,<RSSI>,<SNR>
  int firstComma = rcv.indexOf(',');
  int secondComma = rcv.indexOf(',', firstComma + 1);
  int thirdComma = rcv.indexOf(',', secondComma + 1);
  int fourthComma = rcv.indexOf(',', thirdComma + 1);
  
  if (firstComma < 0 || secondComma < 0 || thirdComma < 0 || fourthComma < 0) {
    Serial.println("  Parse error: invalid format");
    return;
  }
  
  String senderAddr = rcv.substring(5, firstComma);
  String data = rcv.substring(secondComma + 1, thirdComma);
  tank.rssi = rcv.substring(thirdComma + 1, fourthComma).toInt();
  tank.snr = rcv.substring(fourthComma + 1).toInt();
  
  // Verify data format
  if (!data.startsWith("TANK:")) {
    Serial.println("  Not TANK data, ignoring");
    return;
  }
  
  // Parse: TANK:<distance>:<battery%>:<voltage>:<msgId>
  data = data.substring(5);  // Remove "TANK:"
  
  int sep1 = data.indexOf(':');
  int sep2 = data.indexOf(':', sep1 + 1);
  int sep3 = data.indexOf(':', sep2 + 1);
  
  if (sep1 < 0 || sep2 < 0) {
    Serial.println("  Parse error: invalid data format");
    return;
  }
  
  tank.rawDistance = data.substring(0, sep1).toInt();
  tank.batteryPercent = data.substring(sep1 + 1, sep2).toInt();
  tank.batteryVoltage = data.substring(sep2 + 1, sep3 > 0 ? sep3 : data.length()).toFloat();
  
  if (sep3 > 0) {
    tank.msgId = data.substring(sep3 + 1).toInt();
  }
  
  // Send acknowledgment
  sendAck(senderAddr.toInt(), tank.msgId);
  
  // Calculate water level
  calculateWaterLevel();
  
  // Update status
  tank.lastUpdate = millis();
  tank.dataValid = true;
  tank.packetsReceived++;
  
  // Publish to MQTT
  publishMqtt();
  
  // Debug output
  Serial.println("  ────────────────────────────────");
  Serial.println("  Distance:    " + String(tank.rawDistance) + " cm");
  Serial.println("  Water Level: " + String(tank.waterLevel) + " cm (" + 
                 String(tank.waterPercent) + "%)");
  Serial.println("  Battery:     " + String(tank.batteryPercent) + "% (" + 
                 String(tank.batteryVoltage, 2) + "V)");
  Serial.println("  Signal:      " + String(tank.rssi) + " dBm, SNR: " + String(tank.snr));
  Serial.println("  Packets:     " + String(tank.packetsReceived));
  Serial.println("  ────────────────────────────────");
}

// =============================================================================
// SEND ACK TO TRANSMITTER
// =============================================================================
void sendAck(int toAddress, unsigned long msgId) {
  String ack = "ACK:" + String(msgId);
  String cmd = "AT+SEND=" + String(toAddress) + "," + String(ack.length()) + "," + ack;
  sendLoraCommand(cmd);
  Serial.println("  ACK sent: " + ack);
}

// =============================================================================
// CALCULATE WATER LEVEL
// =============================================================================
void calculateWaterLevel() {
  if (tank.rawDistance < 0) {
    tank.waterLevel = -1;
    tank.waterPercent = -1;
    return;
  }
  
  // Calculate water level
  // maxDistance = distance from sensor to empty tank bottom
  int maxDistance = SENSOR_OFFSET + TANK_DEPTH;
  
  // waterLevel = height of water in tank
  tank.waterLevel = maxDistance - tank.rawDistance;
  tank.waterLevel = constrain(tank.waterLevel, 0, TANK_DEPTH);
  
  // Calculate percentage
  tank.waterPercent = (tank.waterLevel * 100) / TANK_DEPTH;
  tank.waterPercent = constrain(tank.waterPercent, 0, 100);
}

// =============================================================================
// PUBLISH DATA TO MQTT
// =============================================================================
void publishMqtt() {
  if (!mqtt.connected()) return;
  
  mqtt.publish("tank/water_level", String(tank.waterLevel).c_str(), true);
  mqtt.publish("tank/water_percent", String(tank.waterPercent).c_str(), true);
  mqtt.publish("tank/raw_distance", String(tank.rawDistance).c_str(), true);
  mqtt.publish("tank/battery_percent", String(tank.batteryPercent).c_str(), true);
  mqtt.publish("tank/battery_voltage", String(tank.batteryVoltage, 2).c_str(), true);
  mqtt.publish("tank/rssi", String(tank.rssi).c_str(), true);
  mqtt.publish("tank/snr", String(tank.snr).c_str(), true);
  mqtt.publish("tank/packets_received", String(tank.packetsReceived).c_str(), true);
  
  // Connection status string
  String status = "unknown";
  switch (connState) {
    case STATE_CONNECTED: status = "connected"; break;
    case STATE_STALE: status = "stale"; break;
    case STATE_LOST: status = "lost"; break;
    case STATE_WAITING: status = "waiting"; break;
    default: status = "starting"; break;
  }
  mqtt.publish("tank/status", status.c_str(), true);
  
  Serial.println("  MQTT published");
}

// =============================================================================
// UPDATE CONNECTION STATE
// =============================================================================
void updateConnectionState() {
  if (!tank.dataValid) {
    if (connState != STATE_WAITING && connState != STATE_STARTING) {
      connState = STATE_WAITING;
    }
    return;
  }
  
  unsigned long age = millis() - tank.lastUpdate;
  
  if (age > CONNECTION_TIMEOUT_MS) {
    connState = STATE_LOST;
  } else if (age > DATA_TIMEOUT_MS) {
    connState = STATE_STALE;
  } else {
    connState = STATE_CONNECTED;
  }
}

// =============================================================================
// UPDATE ALL LEDS
// =============================================================================
void updateAllLeds() {
  updateStatusLed();
  updateWaterLed();
  updateSignalLed();
  strip.show();
}

// =============================================================================
// UPDATE STATUS LED
// =============================================================================
void updateStatusLed() {
  switch (connState) {
    case STATE_STARTING:
      setLed(LED_STATUS, COLOR_YELLOW);
      break;
      
    case STATE_WAITING:
      // Slow blue pulse
      if (millis() - lastStatusBlink > 1000) {
        lastStatusBlink = millis();
        statusBlinkOn = !statusBlinkOn;
      }
      setLed(LED_STATUS, statusBlinkOn ? COLOR_BLUE : COLOR_OFF);
      break;
      
    case STATE_CONNECTED:
      // Solid green with occasional blink
      if (millis() - lastStatusBlink > 3000) {
        lastStatusBlink = millis();
        statusBlinkOn = true;
      }
      if (statusBlinkOn && millis() - lastStatusBlink < 100) {
        setLed(LED_STATUS, COLOR_WHITE);
      } else {
        setLed(LED_STATUS, COLOR_GREEN);
        statusBlinkOn = false;
      }
      break;
      
    case STATE_STALE:
      // Blinking yellow
      if (millis() - lastStatusBlink > 500) {
        lastStatusBlink = millis();
        statusBlinkOn = !statusBlinkOn;
      }
      setLed(LED_STATUS, statusBlinkOn ? COLOR_YELLOW : COLOR_OFF);
      break;
      
    case STATE_LOST:
      // Fast blinking red
      if (millis() - lastStatusBlink > 250) {
        lastStatusBlink = millis();
        statusBlinkOn = !statusBlinkOn;
      }
      setLed(LED_STATUS, statusBlinkOn ? COLOR_RED : COLOR_OFF);
      break;
  }
}

// =============================================================================
// UPDATE WATER LEVEL LED
// =============================================================================
void updateWaterLed() {
  if (!tank.dataValid || tank.waterPercent < 0) {
    setLed(LED_WATER, COLOR_MAGENTA);
    return;
  }
  
  if (tank.waterPercent >= 75) {
    setLed(LED_WATER, COLOR_GREEN);
  } else if (tank.waterPercent >= 50) {
    setLed(LED_WATER, COLOR_CYAN);
  } else if (tank.waterPercent >= 25) {
    setLed(LED_WATER, COLOR_YELLOW);
  } else if (tank.waterPercent >= 10) {
    setLed(LED_WATER, COLOR_ORANGE);
  } else {
    // Critical - blink red
    setLed(LED_WATER, (millis() % 500 < 250) ? COLOR_RED : COLOR_OFF);
  }
}

// =============================================================================
// UPDATE SIGNAL STRENGTH LED
// =============================================================================
void updateSignalLed() {
  if (!tank.dataValid) {
    setLed(LED_SIGNAL, COLOR_OFF);
    return;
  }
  
  // RSSI ranges: -30 (excellent) to -120 (very weak)
  if (tank.rssi > -60) {
    setLed(LED_SIGNAL, COLOR_GREEN);      // Excellent
  } else if (tank.rssi > -80) {
    setLed(LED_SIGNAL, COLOR_CYAN);       // Good
  } else if (tank.rssi > -100) {
    setLed(LED_SIGNAL, COLOR_YELLOW);     // Fair
  } else {
    setLed(LED_SIGNAL, COLOR_RED);        // Weak
  }
}

// =============================================================================
// LED HELPER FUNCTIONS
// =============================================================================
void setLed(int index, uint32_t color) {
  strip.setPixelColor(index, color);
}

void setAllLeds(uint32_t color) {
  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, color);
  }
  strip.show();
}

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
// SEND LORA COMMAND
// =============================================================================
void sendLoraCommand(String cmd) {
  Serial1.println(cmd);
  delay(100);
  while (Serial1.available()) {
    Serial.println("  LoRa: " + Serial1.readStringUntil('\n'));
  }
}

// =============================================================================
// WEB SERVER SETUP
// =============================================================================
void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
  Serial.println("Web server started on port 80");
}

// =============================================================================
// WEB SERVER - JSON DATA ENDPOINT
// =============================================================================
void handleData() {
  String status = "unknown";
  switch (connState) {
    case STATE_CONNECTED: status = "connected"; break;
    case STATE_STALE: status = "stale"; break;
    case STATE_LOST: status = "lost"; break;
    case STATE_WAITING: status = "waiting"; break;
    default: status = "starting"; break;
  }
  
  String json = "{";
  json += "\"rawDistance\":" + String(tank.rawDistance) + ",";
  json += "\"waterLevel\":" + String(tank.waterLevel) + ",";
  json += "\"waterPercent\":" + String(tank.waterPercent) + ",";
  json += "\"batteryPercent\":" + String(tank.batteryPercent) + ",";
  json += "\"batteryVoltage\":\"" + String(tank.batteryVoltage, 2) + "\",";
  json += "\"rssi\":" + String(tank.rssi) + ",";
  json += "\"snr\":" + String(tank.snr) + ",";
  json += "\"packetsReceived\":" + String(tank.packetsReceived) + ",";
  json += "\"lastUpdate\":" + String(tank.lastUpdate / 1000) + ",";
  json += "\"dataValid\":" + String(tank.dataValid ? "true" : "false") + ",";
  json += "\"status\":\"" + status + "\"";
  json += "}";
  
  server.send(200, "application/json", json);
}

// =============================================================================
// WEB SERVER - MAIN PAGE
// =============================================================================
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Water Tank Monitor</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
      min-height: 100vh;
      color: #fff;
      padding: 20px;
    }
    .container { max-width: 400px; margin: 0 auto; }
    h1 {
      text-align: center;
      margin-bottom: 20px;
      font-size: 1.5em;
      color: #00d4ff;
    }
    .status-bar {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 10px 15px;
      background: rgba(0,0,0,0.3);
      border-radius: 10px;
      margin-bottom: 20px;
    }
    .status-dot {
      width: 12px;
      height: 12px;
      border-radius: 50%;
      margin-right: 8px;
      display: inline-block;
    }
    .status-dot.connected { background: #4caf50; box-shadow: 0 0 10px #4caf50; }
    .status-dot.stale { background: #ff9800; animation: pulse 1s infinite; }
    .status-dot.lost { background: #f44336; animation: pulse 0.5s infinite; }
    .status-dot.waiting { background: #2196f3; animation: pulse 2s infinite; }
    @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.3; } }
    .card {
      background: rgba(255,255,255,0.1);
      border-radius: 20px;
      padding: 25px;
      margin-bottom: 20px;
      backdrop-filter: blur(10px);
    }
    .tank-visual {
      position: relative;
      width: 120px;
      height: 200px;
      margin: 0 auto 20px;
      border: 4px solid #00d4ff;
      border-radius: 0 0 20px 20px;
      border-top: none;
      overflow: hidden;
    }
    .tank-visual::before {
      content: '';
      position: absolute;
      top: -4px;
      left: -15px;
      right: -15px;
      height: 20px;
      background: #00d4ff;
      border-radius: 10px 10px 0 0;
    }
    .water {
      position: absolute;
      bottom: 0;
      left: 0;
      right: 0;
      background: linear-gradient(180deg, #00d4ff 0%, #0051ff 100%);
      transition: height 1s ease-out;
      border-radius: 0 0 16px 16px;
    }
    .water-percent {
      position: absolute;
      top: 50%;
      left: 50%;
      transform: translate(-50%, -50%);
      font-size: 2em;
      font-weight: bold;
      text-shadow: 2px 2px 4px rgba(0,0,0,0.5);
      z-index: 10;
    }
    .stats { display: grid; grid-template-columns: 1fr 1fr; gap: 15px; }
    .stat {
      text-align: center;
      padding: 15px;
      background: rgba(0,0,0,0.2);
      border-radius: 15px;
    }
    .stat-value { font-size: 1.8em; font-weight: bold; color: #00d4ff; }
    .stat-label { font-size: 0.8em; color: #aaa; margin-top: 5px; }
    .indicators {
      display: flex;
      justify-content: space-around;
      margin-top: 20px;
      padding-top: 20px;
      border-top: 1px solid rgba(255,255,255,0.1);
    }
    .indicator { text-align: center; }
    .indicator-led {
      width: 20px;
      height: 20px;
      border-radius: 50%;
      margin: 0 auto 5px;
      border: 2px solid rgba(255,255,255,0.3);
    }
    .indicator-label { font-size: 0.7em; color: #888; }
    .battery-bar {
      display: flex;
      align-items: center;
      gap: 10px;
      padding: 15px;
      background: rgba(0,0,0,0.2);
      border-radius: 15px;
      margin-top: 15px;
    }
    .battery-icon {
      width: 40px;
      height: 20px;
      border: 2px solid #fff;
      border-radius: 3px;
      position: relative;
    }
    .battery-icon::after {
      content: '';
      position: absolute;
      right: -6px;
      top: 5px;
      width: 4px;
      height: 8px;
      background: #fff;
      border-radius: 0 2px 2px 0;
    }
    .battery-fill {
      height: 100%;
      border-radius: 1px;
      transition: width 0.5s, background 0.5s;
    }
    .footer {
      text-align: center;
      margin-top: 15px;
      font-size: 0.8em;
      color: #666;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>💧 Water Tank Monitor</h1>
    
    <div class="status-bar">
      <div>
        <span class="status-dot waiting" id="statusDot"></span>
        <span id="statusText">Connecting...</span>
      </div>
      <div id="lastUpdate">--</div>
    </div>
    
    <div class="card">
      <div class="tank-visual">
        <div class="water" id="water"></div>
        <div class="water-percent" id="percent">--%</div>
      </div>
      
      <div class="stats">
        <div class="stat">
          <div class="stat-value" id="level">--</div>
          <div class="stat-label">Water Level (cm)</div>
        </div>
        <div class="stat">
          <div class="stat-value" id="distance">--</div>
          <div class="stat-label">Sensor Distance (cm)</div>
        </div>
      </div>
      
      <div class="battery-bar">
        <div class="battery-icon">
          <div class="battery-fill" id="batteryFill"></div>
        </div>
        <span id="batteryText">--% (--V)</span>
      </div>
      
      <div class="indicators">
        <div class="indicator">
          <div class="indicator-led" id="ledStatus"></div>
          <div class="indicator-label">Status</div>
        </div>
        <div class="indicator">
          <div class="indicator-led" id="ledWater"></div>
          <div class="indicator-label">Water</div>
        </div>
        <div class="indicator">
          <div class="indicator-led" id="ledSignal"></div>
          <div class="indicator-label">Signal</div>
        </div>
      </div>
    </div>
    
    <div class="footer">
      <div>RSSI: <span id="rssi">--</span> dBm | SNR: <span id="snr">--</span></div>
      <div>Packets: <span id="packets">0</span></div>
    </div>
  </div>
  
  <script>
    function updateData() {
      fetch('/data')
        .then(r => r.json())
        .then(d => {
          // Water level
          document.getElementById('percent').textContent = d.waterPercent + '%';
          document.getElementById('water').style.height = Math.max(0, d.waterPercent) + '%';
          document.getElementById('level').textContent = d.waterLevel;
          document.getElementById('distance').textContent = d.rawDistance;
          
          // Battery
          document.getElementById('batteryText').textContent = d.batteryPercent + '% (' + d.batteryVoltage + 'V)';
          let bf = document.getElementById('batteryFill');
          bf.style.width = d.batteryPercent + '%';
          bf.style.background = d.batteryPercent < 20 ? '#f44336' : d.batteryPercent < 50 ? '#ff9800' : '#4caf50';
          
          // Signal
          document.getElementById('rssi').textContent = d.rssi;
          document.getElementById('snr').textContent = d.snr;
          document.getElementById('packets').textContent = d.packetsReceived;
          
          // Status
          let dot = document.getElementById('statusDot');
          let txt = document.getElementById('statusText');
          dot.className = 'status-dot ' + d.status;
          txt.textContent = d.status.charAt(0).toUpperCase() + d.status.slice(1);
          
          // Last update
          if (d.dataValid) {
            let age = Math.round((Date.now()/1000) - d.lastUpdate);
            document.getElementById('lastUpdate').textContent = age + 's ago';
          }
          
          // Water color
          let w = document.getElementById('water');
          if (d.waterPercent >= 75) w.style.background = 'linear-gradient(180deg, #4caf50 0%, #2e7d32 100%)';
          else if (d.waterPercent >= 50) w.style.background = 'linear-gradient(180deg, #00bcd4 0%, #006064 100%)';
          else if (d.waterPercent >= 25) w.style.background = 'linear-gradient(180deg, #ffeb3b 0%, #ff9800 100%)';
          else w.style.background = 'linear-gradient(180deg, #ff5722 0%, #c62828 100%)';
          
          // LED indicators
          let ledStatus = document.getElementById('ledStatus');
          let ledWater = document.getElementById('ledWater');
          let ledSignal = document.getElementById('ledSignal');
          
          // Status LED
          switch(d.status) {
            case 'connected': ledStatus.style.background = '#4caf50'; ledStatus.style.boxShadow = '0 0 10px #4caf50'; break;
            case 'stale': ledStatus.style.background = '#ff9800'; ledStatus.style.boxShadow = '0 0 10px #ff9800'; break;
            case 'lost': ledStatus.style.background = '#f44336'; ledStatus.style.boxShadow = '0 0 10px #f44336'; break;
            default: ledStatus.style.background = '#2196f3'; ledStatus.style.boxShadow = '0 0 10px #2196f3';
          }
          
          // Water LED
          if (d.waterPercent >= 75) { ledWater.style.background = '#4caf50'; ledWater.style.boxShadow = '0 0 10px #4caf50'; }
          else if (d.waterPercent >= 50) { ledWater.style.background = '#00bcd4'; ledWater.style.boxShadow = '0 0 10px #00bcd4'; }
          else if (d.waterPercent >= 25) { ledWater.style.background = '#ffeb3b'; ledWater.style.boxShadow = '0 0 10px #ffeb3b'; }
          else { ledWater.style.background = '#f44336'; ledWater.style.boxShadow = '0 0 10px #f44336'; }
          
          // Signal LED
          if (d.rssi > -60) { ledSignal.style.background = '#4caf50'; ledSignal.style.boxShadow = '0 0 10px #4caf50'; }
          else if (d.rssi > -80) { ledSignal.style.background = '#00bcd4'; ledSignal.style.boxShadow = '0 0 10px #00bcd4'; }
          else if (d.rssi > -100) { ledSignal.style.background = '#ffeb3b'; ledSignal.style.boxShadow = '0 0 10px #ffeb3b'; }
          else { ledSignal.style.background = '#f44336'; ledSignal.style.boxShadow = '0 0 10px #f44336'; }
        })
        .catch(e => {
          document.getElementById('statusDot').className = 'status-dot lost';
          document.getElementById('statusText').textContent = 'Error';
        });
    }
    
    updateData();
    setInterval(updateData, 2000);
  </script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}
