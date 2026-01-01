/**
 * ============================================================================
 * OLED Display Module
 * ============================================================================
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include <Adafruit_SSD1306.h>
#include "config.h"
#include "tank_data.h"

// ============================================================================
// DISPLAY SCREENS
// ============================================================================
enum DisplayScreen {
  SCREEN_WATER,
  SCREEN_BATTERY,
  SCREEN_SIGNAL,
  SCREEN_STATS,
  SCREEN_COUNT
};

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
extern DisplayScreen currentScreen;
extern unsigned long lastScreenChange;
extern unsigned long lastDisplayUpdate;

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================
void initializeDisplay(Adafruit_SSD1306& display);
void updateDisplay(Adafruit_SSD1306& display);
void drawStatusIcons(Adafruit_SSD1306& display);
void drawWaterScreen(Adafruit_SSD1306& display);
void drawBatteryScreen(Adafruit_SSD1306& display);
void drawSignalScreen(Adafruit_SSD1306& display);
void drawStatsScreen(Adafruit_SSD1306& display);
void drawTankGraphic(Adafruit_SSD1306& display, int x, int y, int width, int height, int percent);

#endif // DISPLAY_H
