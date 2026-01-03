/**
 * ============================================================================
 * MQTT Handler Implementation
 * ============================================================================
 */

#include "mqtt_handler.h"
#include "tank_data.h"
#include "lora_comm.h"
#include <Preferences.h>

extern Preferences preferences;

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
String mqtt_server = DEFAULT_MQTT_SERVER;
int mqtt_port = DEFAULT_MQTT_PORT;
String mqtt_user = DEFAULT_MQTT_USER;
String mqtt_password = DEFAULT_MQTT_PASSWORD;
bool mqtt_enabled = DEFAULT_MQTT_ENABLED;

unsigned long lastMqttReconnect = 0;

// ============================================================================
// LOAD MQTT SETTINGS
// ============================================================================
void loadMqttSettings() {
  mqtt_server = preferences.getString("mqtt_server", DEFAULT_MQTT_SERVER);
  mqtt_port = preferences.getInt("mqtt_port", DEFAULT_MQTT_PORT);
  mqtt_user = preferences.getString("mqtt_user", DEFAULT_MQTT_USER);
  mqtt_password = preferences.getString("mqtt_pass", DEFAULT_MQTT_PASSWORD);
  mqtt_enabled = preferences.getBool("mqtt_enabled", DEFAULT_MQTT_ENABLED);
}

// ============================================================================
// SAVE MQTT SETTINGS
// ============================================================================
void saveMqttSettings() {
  preferences.putString("mqtt_server", mqtt_server);
  preferences.putInt("mqtt_port", mqtt_port);
  preferences.putString("mqtt_user", mqtt_user);
  preferences.putString("mqtt_pass", mqtt_password);
  preferences.putBool("mqtt_enabled", mqtt_enabled);
}

// ============================================================================
// INITIALIZE MQTT
// ============================================================================
void initializeMqtt(PubSubClient& mqtt) {
  if (mqtt_enabled) {
    mqtt.setServer(mqtt_server.c_str(), mqtt_port);
    mqtt.setBufferSize(256);  // Increased for stability (was 128)
    mqtt.setKeepAlive(15);     // Shorter keep-alive for faster recovery
    delay(10);  // Feed WDT after MQTT init
  }
}

// ============================================================================
// CONNECT TO MQTT (Optimized - no String objects!)
// ============================================================================
void connectMqtt(PubSubClient& mqtt) {
  if (!mqtt_enabled) return;
  if (millis() - lastMqttReconnect < 5000) return;

  lastMqttReconnect = millis();
  yield();  // Feed watchdog before connection attempt

  // Build client ID with snprintf (no String objects)
  char clientId[16];
  snprintf(clientId, sizeof(clientId), "Tank-%04X", random(0xffff));

  yield();  // Feed watchdog before connecting

  bool ok = mqtt_user.length() > 0 ?
            mqtt.connect(clientId, mqtt_user.c_str(), mqtt_password.c_str()) :
            mqtt.connect(clientId);

  yield();  // Feed watchdog after connection attempt

  if (ok) {
#if DEBUG_VERBOSE
    Serial.println("MQTT connected");
#endif
    yield();  // Feed watchdog before publishing
    publishMqttData(mqtt);  // This already has internal yield() calls
    yield();  // Feed watchdog after publishing
  }
}

// ============================================================================
// PUBLISH DATA TO MQTT (Optimized - no String objects!)
// ============================================================================
void publishMqttData(PubSubClient& mqtt) {
  if (!tank.dataValid || !mqtt_enabled || !mqtt.connected()) return;

  delay(10);  // CRITICAL: Feed hardware WDT before publishing

  // Use a single buffer for all conversions (no heap allocations!)
  char buf[32];

  // Water level (integer)
  snprintf(buf, sizeof(buf), "%d", tank.waterLevel);
  mqtt.publish("tank/water_level", buf, true);
  delay(1);  // Feed hardware WDT

  // Water percent (integer)
  snprintf(buf, sizeof(buf), "%d", tank.waterPercent);
  mqtt.publish("tank/water_percent", buf, true);
  delay(1);  // Feed hardware WDT

  // Water liters (float with 1 decimal)
  snprintf(buf, sizeof(buf), "%.1f", tank.waterLiters);
  mqtt.publish("tank/water_liters", buf, true);
  delay(1);  // Feed hardware WDT

  // Battery percent (integer)
  snprintf(buf, sizeof(buf), "%d", tank.batteryPercent);
  mqtt.publish("tank/battery_percent", buf, true);
  delay(1);  // Feed hardware WDT

  // Battery voltage (float with 2 decimals)
  snprintf(buf, sizeof(buf), "%.2f", tank.batteryVoltage);
  mqtt.publish("tank/battery_voltage", buf, true);
  delay(1);  // Feed hardware WDT

  // RSSI (integer)
  snprintf(buf, sizeof(buf), "%d", tank.rssi);
  mqtt.publish("tank/rssi", buf, true);
  delay(1);  // Feed hardware WDT

  // SNR (integer)
  snprintf(buf, sizeof(buf), "%d", tank.snr);
  mqtt.publish("tank/snr", buf, true);
  delay(1);  // Feed hardware WDT

  // Status (string - already c_str)
  mqtt.publish("tank/status", getLoRaStateString().c_str(), true);
  delay(1);  // Feed hardware WDT

  // Packets received (integer)
  snprintf(buf, sizeof(buf), "%d", tank.packetsReceived);
  mqtt.publish("tank/packets_received", buf, true);
  delay(1);  // Feed hardware WDT

#if DEBUG_VERBOSE
  Serial.println("MQTT published");
#endif
  delay(10);  // CRITICAL: Extra delay after all publishes before returning
}
