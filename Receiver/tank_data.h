/**
 * ============================================================================
 * Tank Data Structures and Calculations
 * ============================================================================
 */

#ifndef TANK_DATA_H
#define TANK_DATA_H

#include <Arduino.h>
#include "config.h"

// ============================================================================
// TANK DATA STRUCTURE
// ============================================================================
struct TankData {
  int rawDistance       = 0;
  int waterLevel        = 0;
  int waterPercent      = 0;
  float waterLiters     = 0.0;
  float tankCapacity    = 0.0;
  int batteryPercent    = 0;
  float batteryVoltage  = 0.0;
  int rssi              = 0;
  int snr               = 0;
  unsigned long lastUpdate = 0;
  unsigned long messageId  = 0;
  bool dataValid        = false;
  int packetsReceived   = 0;
};

// ============================================================================
// TANK SETTINGS (Global Variables)
// ============================================================================
extern int MIN_DISTANCE_CM;      // Sensor reading when tank is FULL
extern int MAX_DISTANCE_CM;      // Sensor reading when tank is EMPTY
extern float TANK_CAPACITY_LITERS;

// ============================================================================
// GLOBAL TANK DATA INSTANCE
// ============================================================================
extern TankData tank;

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================
void initializeTankSettings();
float calculateWaterVolume(int waterLevelCm);
void calculateWaterLevel();
void loadTankSettings();
void saveTankSettings(int minDistance, int maxDistance, float capacity);

#endif // TANK_DATA_H
