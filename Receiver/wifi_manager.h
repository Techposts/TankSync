/**
 * ============================================================================
 * WiFi and Web Server Module
 * ============================================================================
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "config.h"

// ============================================================================
// WIFI STATE
// ============================================================================
enum WiFiState {
  WIFI_STATE_DISCONNECTED,
  WIFI_STATE_CONNECTING,
  WIFI_STATE_CONNECTED,
  WIFI_STATE_AP_MODE
};

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
extern WiFiState wifiState;
extern String savedSSID;
extern String savedPassword;
extern unsigned long lastWiFiRetry;
extern int wifiRetryCount;

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================
void loadWiFiCredentials();
void saveWiFiCredentials();
void clearWiFiCredentials();
void loadAlertSettings();
void saveAlertSettings();
void startWiFiConnectionBlocking();
void startWiFiConnectionNonBlocking();
void updateWiFiConnection();
void startAPMode();
void checkWiFiStatus();
WiFiState getWiFiState();

// Web Server
void setupWebServer(WebServer& server);
void handleRoot(WebServer& server);
void handleSetup(WebServer& server);
void handleScan(WebServer& server);
void handleConnect(WebServer& server);
void handleData(WebServer& server);
void handleSaveSettings(WebServer& server);
void handleWifi(WebServer& server);
void handleMqtt(WebServer& server);
void handleMqttSave(WebServer& server);
void handleLora(WebServer& server);
void handleLoraSave(WebServer& server);
void handleAlerts(WebServer& server);
void handleAlertsSave(WebServer& server);
void handleReset(WebServer& server);

// External declarations from other modules
extern String mqtt_server;
extern int mqtt_port;
extern String mqtt_user;
extern String mqtt_password;
extern bool mqtt_enabled;

extern bool alerts_enabled;
extern int alert_low_water;
extern int alert_low_battery;
extern String alert_email;

void saveMqttSettings();
void saveLoRaSettings(String freq, int netid, int addr);
void saveAlertSettings();

#endif // WIFI_MANAGER_H
