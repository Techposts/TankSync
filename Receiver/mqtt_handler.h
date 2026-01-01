/**
 * ============================================================================
 * MQTT Handler Module
 * ============================================================================
 */

#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include <Arduino.h>
#include <PubSubClient.h>
#include "config.h"

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
extern String mqtt_server;
extern int mqtt_port;
extern String mqtt_user;
extern String mqtt_password;
extern bool mqtt_enabled;

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================
void loadMqttSettings();
void saveMqttSettings();
void initializeMqtt(PubSubClient& mqtt);
void connectMqtt(PubSubClient& mqtt);
void publishMqttData(PubSubClient& mqtt);

#endif // MQTT_HANDLER_H
