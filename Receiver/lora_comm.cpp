/**
 * ============================================================================
 * LoRa Communication Implementation
 * ============================================================================
 */

#include "lora_comm.h"
#include <Preferences.h>

extern Preferences preferences;

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
ConnectionState loraState = STATE_STARTING;
bool loraHardwareConnected = false;
bool newDataReceived = false;  // Flag for MQTT publishing
bool loraProcessing = false;   // Flag to prevent WiFi operations during LoRa processing

// LoRa runtime settings
String LORA_FREQUENCY = DEFAULT_LORA_FREQUENCY;
int LORA_NETWORK_ID = DEFAULT_LORA_NETWORK_ID;
int MY_ADDRESS = DEFAULT_MY_ADDRESS;

// ============================================================================
// LOAD LORA SETTINGS
// ============================================================================
void loadLoRaSettings() {
  LORA_FREQUENCY = preferences.getString("lora_freq", DEFAULT_LORA_FREQUENCY);
  LORA_NETWORK_ID = preferences.getInt("lora_netid", DEFAULT_LORA_NETWORK_ID);
  MY_ADDRESS = preferences.getInt("lora_addr", DEFAULT_MY_ADDRESS);
}

// ============================================================================
// SAVE LORA SETTINGS
// ============================================================================
void saveLoRaSettings(String freq, int netid, int addr) {
  LORA_FREQUENCY = freq;
  LORA_NETWORK_ID = netid;
  MY_ADDRESS = addr;

  preferences.putString("lora_freq", LORA_FREQUENCY);
  preferences.putInt("lora_netid", LORA_NETWORK_ID);
  preferences.putInt("lora_addr", MY_ADDRESS);
}

// ============================================================================
// INITIALIZE LORA MODULE
// ============================================================================
void initializeLoRa() {
  Serial1.begin(LORA_BAUD_RATE, SERIAL_8N1, LORA_RX_PIN, LORA_TX_PIN);
  delay(500);
  yield();  // Feed watchdog

  while (Serial1.available()) Serial1.read();

  // Test if module is responding
  bool atOk = sendLoRaCommand("AT");

  if (!atOk) {
    Serial.println("ERROR: LoRa hardware not detected!");
#if DEBUG_VERBOSE
    Serial.println("Check: GPIO20->RX, GPIO21->TX, Power, GND");
#endif
    loraHardwareConnected = false;
    loraState = STATE_LOST;
    return;
  }

  // Configure module
  bool addrOk = sendLoRaCommand("AT+ADDRESS=" + String(MY_ADDRESS));
  bool netOk = sendLoRaCommand("AT+NETWORKID=" + String(LORA_NETWORK_ID));
  bool bandOk = sendLoRaCommand("AT+BAND=" + String(LORA_FREQUENCY));

  if (addrOk && netOk && bandOk) {
    Serial.println("LoRa OK");
    loraHardwareConnected = true;
    loraState = STATE_WAITING;
  } else {
    Serial.println("ERROR: LoRa config failed");
    loraHardwareConnected = false;
    loraState = STATE_LOST;
  }
}

// ============================================================================
// SEND LORA AT COMMAND
// ============================================================================
bool sendLoRaCommand(String cmd) {
  // Clear any pending data
  while (Serial1.available()) Serial1.read();

  // Send command
  Serial1.println(cmd);

  // Wait for response (RYLR998 responds with "+OK" or "+ERR")
  unsigned long start = millis();
  String response = "";

  while (millis() - start < 1000) {  // 1 second timeout
    if (Serial1.available()) {
      char c = Serial1.read();
      response += c;

      // Check if we got a complete response
      if (response.indexOf("+OK") >= 0) {
#if DEBUG_VERBOSE
        Serial.printf("%s OK\n", cmd.c_str());
#endif
        return true;
      }
      if (response.indexOf("+ERR") >= 0) {
#if DEBUG_VERBOSE
        Serial.printf("%s ERROR\n", cmd.c_str());
#endif
        return false;
      }
    }
    yield();  // Feed watchdog during wait
    delay(10);
  }

  // Timeout - no response received
#if DEBUG_VERBOSE
  Serial.printf("%s TIMEOUT\n", cmd.c_str());
#endif
  return false;
}

// ============================================================================
// PROCESS LORA DATA
// ============================================================================
void processLoRaData() {
  if (Serial1.available()) {
    loraProcessing = true;  // Signal that LoRa is processing - defer WiFi operations
  }

  while (Serial1.available()) {
    String msg = Serial1.readStringUntil('\n');
    msg.trim();
    if (msg.startsWith("+RCV=")) parseReceivedMessage(msg);
    yield();  // Feed watchdog during LoRa data processing
  }

  loraProcessing = false;  // Done processing
}

// ============================================================================
// PARSE RECEIVED MESSAGE (Optimized with strtok - no String objects!)
// ============================================================================
void parseReceivedMessage(String message) {
  delay(5);  // Feed hardware WDT at start
  Serial.println("RX: " + message);
  delay(1);

  // Convert to C-string for in-place parsing (prevents memory fragmentation)
  char buf[128];
  message.toCharArray(buf, sizeof(buf));
  delay(1);

  // Skip "+RCV=" prefix
  char* p = buf + 5;

  // Parse: sender,length,data,rssi,snr
  char* sender = strtok(p, ",");
  char* length = strtok(NULL, ",");
  char* data = strtok(NULL, ",");
  char* rssiStr = strtok(NULL, ",");
  char* snrStr = strtok(NULL, ",\r\n");

  if (!sender || !data || !rssiStr || !snrStr) {
    delay(1);
    return;
  }

  delay(2);  // Feed watchdog during parsing

  tank.rssi = atoi(rssiStr);
  tank.snr = atoi(snrStr);
  delay(1);

  // Check if data starts with "TANK:"
  if (strncmp(data, "TANK:", 5) != 0) {
    delay(1);
    return;
  }
  data += 5;  // Skip "TANK:" prefix
  delay(1);

  // Parse: distance:battery:voltage:msgId
  char* distStr = strtok(data, ":");
  char* batStr = strtok(NULL, ":");
  char* voltStr = strtok(NULL, ":");
  char* msgIdStr = strtok(NULL, ":\r\n");

  if (!distStr || !batStr || !voltStr || !msgIdStr) {
    delay(1);
    return;
  }

  delay(2);  // Feed watchdog during parsing

  tank.rawDistance = atoi(distStr);
  tank.batteryPercent = atoi(batStr);
  tank.batteryVoltage = atof(voltStr);
  tank.messageId = atol(msgIdStr);
  delay(1);

  if (tank.rawDistance < SENSOR_MIN_READING || tank.rawDistance > SENSOR_MAX_READING) {
#if DEBUG_VERBOSE
    Serial.println("Invalid sensor reading");
#endif
    delay(1);
    return;
  }

  delay(5);  // Feed watchdog before ACK (CRITICAL - ACK takes time)
  sendAck(atoi(sender), tank.messageId);  // This now has internal delay() calls
  delay(5);  // Feed watchdog after ACK (CRITICAL)

  calculateWaterLevel();
  delay(2);  // Feed watchdog after calculation

  tank.lastUpdate = millis();
  tank.dataValid = true;
  tank.packetsReceived++;
  newDataReceived = true;  // Signal main loop to publish MQTT
  delay(1);

  // Simplified output: Water%, Liters, Battery%, RSSI
  Serial.printf("%d%% %.0fL Bat:%d%% R:%d\n",
                tank.waterPercent, tank.waterLiters, tank.batteryPercent, tank.rssi);
  delay(2);  // Feed watchdog at end of parsing
}

// ============================================================================
// SEND ACK TO TRANSMITTER (Optimized - no String objects!)
// ============================================================================
void sendAck(int addr, unsigned long id) {
  delay(1);  // Feed hardware WDT before ACK

  // Build ACK message with snprintf (no heap allocations)
  char ackData[20];
  snprintf(ackData, sizeof(ackData), "ACK:%lu", id);

  int ackLen = strlen(ackData);

  // Build AT command with snprintf (no heap allocations)
  char cmd[64];
  snprintf(cmd, sizeof(cmd), "AT+SEND=%d,%d,%s", addr, ackLen, ackData);

  delay(1);  // Feed hardware WDT before sending
  Serial1.println(cmd);
  delay(100);  // This also feeds WDT
  delay(1);  // Extra WDT feed after delay

  while (Serial1.available()) {
    Serial1.read();
    delay(1);  // Feed hardware WDT while clearing buffer (safer than yield)
  }
}

// ============================================================================
// UPDATE LORA STATE
// ============================================================================
void updateLoRaState() {
  if (!loraHardwareConnected) {
    loraState = STATE_LOST;
    return;
  }
  if (!tank.dataValid) {
    loraState = STATE_WAITING;
    return;
  }
  unsigned long age = millis() - tank.lastUpdate;
  if (age > DATA_LOST_MS) loraState = STATE_LOST;
  else if (age > DATA_STALE_MS) loraState = STATE_STALE;
  else loraState = STATE_CONNECTED;
}

// ============================================================================
// GET LORA STATE STRING
// ============================================================================
String getLoRaStateString() {
  switch (loraState) {
    case STATE_CONNECTED: return "OK";
    case STATE_STALE: return "Stale";
    case STATE_LOST: return "Lost";
    default: return "Wait";
  }
}
