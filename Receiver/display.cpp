/**
 * ============================================================================
 * OLED Display Implementation
 * ============================================================================
 */

#include "display.h"
#include "lora_comm.h"
#include "wifi_manager.h"
#include <WiFi.h>

extern bool blinkState;

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
DisplayScreen currentScreen = SCREEN_WATER;
unsigned long lastScreenChange = 0;
unsigned long lastDisplayUpdate = 0;

// ============================================================================
// STATUS ICONS (8x8 bitmaps)
// ============================================================================
const unsigned char PROGMEM icon_wifi_on[] = { 0x00, 0x3C, 0x42, 0x18, 0x24, 0x00, 0x18, 0x00 };
const unsigned char PROGMEM icon_wifi_ap[] = { 0x00, 0x3C, 0x42, 0x5A, 0x24, 0x18, 0x18, 0x00 };
const unsigned char PROGMEM icon_wifi_off[] = { 0x00, 0x3C, 0x42, 0x19, 0x26, 0x08, 0x10, 0x00 };
const unsigned char PROGMEM icon_lora_on[] = { 0x18, 0x18, 0x18, 0x3C, 0x7E, 0x18, 0x18, 0x18 };
const unsigned char PROGMEM icon_lora_wait[] = { 0x18, 0x18, 0x00, 0x3C, 0x7E, 0x00, 0x18, 0x18 };

// ============================================================================
// INITIALIZE DISPLAY
// ============================================================================
void initializeDisplay(Adafruit_SSD1306& display) {
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("✗ OLED failed!");
    return;
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.display();
  Serial.println("✓ OLED OK");
}

// ============================================================================
// UPDATE DISPLAY (Main function called from loop)
// ============================================================================
void updateDisplay(Adafruit_SSD1306& display) {
  unsigned long now = millis();

  // Rotate screens every SCREEN_ROTATE_MS
  if (now - lastScreenChange >= SCREEN_ROTATE_MS) {
    lastScreenChange = now;
    currentScreen = (DisplayScreen)((currentScreen + 1) % SCREEN_COUNT);
  }

  // Update display at reduced rate (250ms = 4 FPS) to save CPU for WiFi/MQTT
  if (now - lastDisplayUpdate >= DISPLAY_UPDATE_MS) {
    lastDisplayUpdate = now;

    display.clearDisplay();
    drawStatusIcons(display);
    yield();  // Feed watchdog after drawing icons

    switch (currentScreen) {
      case SCREEN_WATER: drawWaterScreen(display); break;
      case SCREEN_BATTERY: drawBatteryScreen(display); break;
      case SCREEN_SIGNAL: drawSignalScreen(display); break;
      case SCREEN_STATS: drawStatsScreen(display); break;
    }

    yield();  // Feed watchdog before display update
    display.display();
  }
}

// ============================================================================
// DRAW STATUS ICONS (Top bar)
// ============================================================================
void drawStatusIcons(Adafruit_SSD1306& display) {
  display.fillRect(0, 0, SCREEN_WIDTH, 10, SSD1306_BLACK);

  // WiFi icon
  WiFiState state = getWiFiState();
  if (state == WIFI_STATE_CONNECTED) {
    display.drawBitmap(2, 1, icon_wifi_on, 8, 8, SSD1306_WHITE);
  } else if (state == WIFI_STATE_AP_MODE) {
    if (blinkState) display.drawBitmap(2, 1, icon_wifi_ap, 8, 8, SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(12, 1);
    display.print("AP");
  } else {
    display.drawBitmap(2, 1, icon_wifi_off, 8, 8, SSD1306_WHITE);
  }

  // LoRa icon
  if (!loraHardwareConnected) {
    display.setTextSize(1);
    display.setCursor(44, 1);
    display.print("X");
  } else if (loraState == STATE_CONNECTED) {
    display.drawBitmap(44, 1, icon_lora_on, 8, 8, SSD1306_WHITE);
  } else if (blinkState) {
    display.drawBitmap(44, 1, icon_lora_wait, 8, 8, SSD1306_WHITE);
  }

  // Water percentage
  display.setTextSize(1);
  if (tank.dataValid) {
    display.setCursor(85, 1);
    display.print(tank.waterPercent);
    display.print("%");
    display.drawRect(118, 1, 8, 8, SSD1306_WHITE);
    int fillH = map(constrain(tank.waterPercent, 0, 100), 0, 100, 0, 6);
    if (fillH > 0) display.fillRect(119, 8 - fillH, 6, fillH, SSD1306_WHITE);
  } else {
    display.setCursor(100, 1);
    display.print("---%");
  }

  display.drawFastHLine(0, 10, SCREEN_WIDTH, SSD1306_WHITE);

  // Navigation dots - CENTERED at bottom
  int dotStartX = SCREEN_WIDTH/2 - (SCREEN_COUNT*10)/2;
  for (int i = 0; i < SCREEN_COUNT; i++) {
    if (i == (int)currentScreen)
      display.fillCircle(dotStartX + i*10, 60, 2, SSD1306_WHITE);
    else
      display.drawCircle(dotStartX + i*10, 60, 1, SSD1306_WHITE);
  }
}

// ============================================================================
// DRAW WATER SCREEN
// ============================================================================
void drawWaterScreen(Adafruit_SSD1306& display) {
  drawTankGraphic(display, 4, 14, 40, 40, tank.dataValid ? tank.waterPercent : 0);

  display.setTextSize(3);
  display.setCursor(50, 16);
  if (tank.dataValid) {
    if (tank.waterPercent < 10) display.setCursor(68, 16);
    else if (tank.waterPercent < 100) display.setCursor(56, 16);
    display.print(tank.waterPercent);
  } else {
    display.print("--");
  }
  display.setTextSize(1);
  display.print("%");

  // Show liters on separate line
  display.setTextSize(1);
  display.setCursor(50, 44);
  if (tank.dataValid) {
    display.print((int)tank.waterLiters);
    display.print("/");
    display.print((int)tank.tankCapacity);
    display.print("L");
  } else {
    display.print("Waiting...");
  }
}

// ============================================================================
// DRAW TANK GRAPHIC (Static - no animation for CPU optimization)
// ============================================================================
void drawTankGraphic(Adafruit_SSD1306& display, int x, int y, int width, int height, int percent) {
  display.drawRect(x, y, width, height, SSD1306_WHITE);
  int capW = width / 2;
  display.fillRect(x + (width - capW) / 2, y - 2, capW, 3, SSD1306_WHITE);

  if (percent > 0) {
    int waterH = map(constrain(percent, 0, 100), 0, 100, 0, height - 4);
    int waterY = y + height - waterH - 2;
    display.fillRect(x + 2, waterY, width - 4, waterH, SSD1306_WHITE);
  }
}

// ============================================================================
// DRAW BATTERY SCREEN
// ============================================================================
void drawBatteryScreen(Adafruit_SSD1306& display) {
  int batX = 20, batY = 16, batW = 50, batH = 24;

  // Draw Battery Outline
  display.drawRect(batX, batY, batW, batH, SSD1306_WHITE);
  display.fillRect(batX + batW, batY + 6, 4, 12, SSD1306_WHITE); // Terminal

  // Draw Level or Question Mark
  if (tank.dataValid) {
    int fillW = map(constrain(tank.batteryPercent, 0, 100), 0, 100, 0, batW - 4);
    if (fillW > 0) display.fillRect(batX + 2, batY + 2, fillW, batH - 4, SSD1306_WHITE);

    // Text below battery
    display.setTextSize(1);
    display.setCursor(batX, batY + batH + 5);
    display.print(tank.batteryPercent);
    display.print("% ");
    display.print(tank.batteryVoltage, 2);
    display.print("V");
  } else {
    // Show '?' if no data
    display.setTextSize(2);
    display.setCursor(batX + 15, batY + 5);
    display.print("?");

    display.setTextSize(1);
    display.setCursor(batX, batY + batH + 5);
    display.print("--% --V");
  }
}

// ============================================================================
// DRAW SIGNAL SCREEN
// ============================================================================
void drawSignalScreen(Adafruit_SSD1306& display) {
  display.setTextSize(2);
  display.setCursor(10, 20);
  if (tank.dataValid) {
    display.print(tank.rssi);
    display.print("dBm");
  } else {
    display.print("--");
  }

  display.setTextSize(1);
  display.setCursor(10, 45);
  display.print("SNR: ");
  if (tank.dataValid) display.print(tank.snr);
  else display.print("--");
}

// ============================================================================
// DRAW STATS SCREEN
// ============================================================================
void drawStatsScreen(Adafruit_SSD1306& display) {
  display.setTextSize(1);

  // Left column
  display.setCursor(0, 15);
  display.print("Pkts: ");
  display.print(tank.packetsReceived);

  display.setCursor(64, 15);
  display.print("Dist: ");
  if (tank.dataValid) {
    display.print(tank.rawDistance);
    display.print("cm");
  } else {
    display.print("--");
  }

  display.setCursor(0, 30);
  display.print("Ago: ");
  if (tank.dataValid) {
    long sec = (millis() - tank.lastUpdate) / 1000;
    if (sec < 60) {
      display.print(sec);
      display.print("s");
    } else {
      display.print(sec / 60);
      display.print("m");
    }
  } else {
    display.print("--");
  }

  display.setCursor(0, 45);
  display.print("IP: ");
  WiFiState state = getWiFiState();
  if (state == WIFI_STATE_CONNECTED) {
    display.print(WiFi.localIP());
  } else if (state == WIFI_STATE_AP_MODE) {
    display.print("192.168.4.1");
  } else {
    display.print("--");
  }
}
