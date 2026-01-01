/**
 * ============================================================================
 * LoRa Communication Module
 * ============================================================================
 */

#ifndef LORA_COMM_H
#define LORA_COMM_H

#include <Arduino.h>
#include "config.h"
#include "tank_data.h"

// ============================================================================
// CONNECTION STATE
// ============================================================================
enum ConnectionState {
  STATE_STARTING,
  STATE_WAITING,
  STATE_CONNECTED,
  STATE_STALE,
  STATE_LOST
};

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
extern ConnectionState loraState;
extern bool loraHardwareConnected;
extern bool newDataReceived;  // Flag for MQTT publishing

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================
void loadLoRaSettings();
void saveLoRaSettings(String freq, int netid, int addr);
void initializeLoRa();
bool sendLoRaCommand(String cmd);
void processLoRaData();
void parseReceivedMessage(String message);
void sendAck(int addr, unsigned long id);
void updateLoRaState();
String getLoRaStateString();

#endif // LORA_COMM_H
