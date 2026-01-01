/**
 * ============================================================================
 * Tank Data Implementation
 * ============================================================================
 */

#include "tank_data.h"
#include <Preferences.h>

extern Preferences preferences;

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
TankData tank;

int MIN_DISTANCE_CM        = DEFAULT_MIN_DISTANCE;    // When tank is FULL
int MAX_DISTANCE_CM        = DEFAULT_MAX_DISTANCE;    // When tank is EMPTY
float TANK_CAPACITY_LITERS = DEFAULT_TANK_CAPACITY;

// ============================================================================
// INITIALIZE TANK SETTINGS
// ============================================================================
void initializeTankSettings() {
  loadTankSettings();
  tank.tankCapacity = TANK_CAPACITY_LITERS;
}

// ============================================================================
// CALCULATE WATER VOLUME (Linear calculation based on water level)
// ============================================================================
float calculateWaterVolume(int waterLevelCm) {
  int totalRange = MAX_DISTANCE_CM - MIN_DISTANCE_CM;
  if (totalRange <= 0) return 0;
  // Linear calculation: (water_level / total_range) * total_capacity
  return (waterLevelCm * TANK_CAPACITY_LITERS) / totalRange;
}

// ============================================================================
// CALCULATE WATER LEVEL FROM DISTANCE
// ============================================================================
void calculateWaterLevel() {
  // Calculate the usable range
  int totalRange = MAX_DISTANCE_CM - MIN_DISTANCE_CM;

  // Calculate water level (how much of the range is filled)
  // When sensor reads MIN_DISTANCE_CM, tank is FULL (100%)
  // When sensor reads MAX_DISTANCE_CM, tank is EMPTY (0%)
  tank.waterLevel = constrain(MAX_DISTANCE_CM - tank.rawDistance, 0, totalRange);

  // Calculate percentage
  if (totalRange > 0) {
    tank.waterPercent = constrain((tank.waterLevel * 100) / totalRange, 0, 100);
  } else {
    tank.waterPercent = 0;
  }

  // Calculate liters
  tank.waterLiters = calculateWaterVolume(tank.waterLevel);
}

// ============================================================================
// LOAD TANK SETTINGS FROM FLASH
// ============================================================================
void loadTankSettings() {
  MIN_DISTANCE_CM = preferences.getInt("min_distance", DEFAULT_MIN_DISTANCE);
  MAX_DISTANCE_CM = preferences.getInt("max_distance", DEFAULT_MAX_DISTANCE);
  TANK_CAPACITY_LITERS = preferences.getFloat("tank_capacity", DEFAULT_TANK_CAPACITY);
}

// ============================================================================
// SAVE TANK SETTINGS TO FLASH
// ============================================================================
void saveTankSettings(int minDistance, int maxDistance, float capacity) {
  // Validation
  if (minDistance >= maxDistance) {
    // Invalid configuration, use defaults
    MIN_DISTANCE_CM = DEFAULT_MIN_DISTANCE;
    MAX_DISTANCE_CM = DEFAULT_MAX_DISTANCE;
  } else {
    MIN_DISTANCE_CM = minDistance;
    MAX_DISTANCE_CM = maxDistance;
  }

  TANK_CAPACITY_LITERS = (capacity < 1.0) ? DEFAULT_TANK_CAPACITY : capacity;

  // Save to flash
  preferences.putInt("min_distance", MIN_DISTANCE_CM);
  preferences.putInt("max_distance", MAX_DISTANCE_CM);
  preferences.putFloat("tank_capacity", TANK_CAPACITY_LITERS);

  // Update tank capacity
  tank.tankCapacity = TANK_CAPACITY_LITERS;
}
