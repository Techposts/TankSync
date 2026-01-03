# LoRa Water Tank Monitor - RECEIVER (Modular Version)

USB-powered receiver unit that receives tank data via LoRa, displays on OLED, controls status LEDs, hosts web dashboard, and publishes to MQTT/Home Assistant.

## Architecture

This is a **modular refactored version** with improved code organization:

```
Receiver.ino         - Main program loop
├── config.h         - Pin definitions and constants
├── tank_data.h/cpp  - Tank data management
├── lora_comm.h/cpp  - LoRa communication
├── display.h/cpp    - OLED display handling
├── wifi_manager.h/cpp - WiFi connection management
└── mqtt_handler.h/cpp - MQTT publishing
```

### Benefits of Modular Design
- Easier to debug and maintain
- Each module has a single responsibility
- Reusable components
- Better for team collaboration
- Clearer code organization

## Hardware

- **MCU**: ESP32-C3 SuperMini
- **LoRa Module**: RYLR998 (868/915 MHz)
- **Display**: SSD1306 OLED (128x64, I2C)
- **Indicators**: WS2812B RGB LED Strip (2 LEDs)
- **Power**: USB 5V

## Pin Connections

### RYLR998 LoRa Module

| ESP32-C3 Pin | RYLR998 Pin | Function | Notes |
|--------------|-------------|----------|-------|
| GPIO21 | RXD (Pin 3) | UART TX | ESP TX → LoRa RX |
| GPIO20 | TXD (Pin 4) | UART RX | ESP RX ← LoRa TX |
| 3.3V | VDD (Pin 1) | Power | |
| GND | GND (Pin 5) | Ground | |

### SSD1306 OLED Display (I2C)

| ESP32-C3 Pin | OLED Pin | Function | Notes |
|--------------|----------|----------|-------|
| GPIO9 | SDA | I2C Data | |
| GPIO10 | SCL | I2C Clock | |
| 3.3V | VCC | Power | |
| GND | GND | Ground | |

**I2C Address**: 0x3C (default for most SSD1306 modules)

### WS2812B LED Strip (2 LEDs)

| ESP32-C3 Pin | LED Pin | Function | Notes |
|--------------|---------|----------|-------|
| GPIO2 | DIN | Data | |
| 3.3V | VCC | Power | USB-powered, always on |
| GND | GND | Ground | |

**LED Configuration**:
- **LED 0** (Water Level): Shows tank water level status
- **LED 1** (System Status): Shows WiFi + LoRa connection status

## Circuit Diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│                      ESP32-C3 SuperMini                              │
│                      (USB Powered - 5V)                              │
│                                                                      │
│  ┌────────────────────────────────────────────────────────────┐    │
│  │                                                             │    │
│  │  [20] RX ──────────────────────────┐                       │    │
│  │  [21] TX ─────────────────┐        │                       │    │
│  │  3.3V ──────┬─────────────┼────────┼──────┐                │    │
│  │  GND ───────┼─────────────┼────────┼──────┼────┐           │    │
│  │             │             │        │      │    │           │    │
│  │  [9] SDA ───│─────────────│────────│──────│────│───┐       │    │
│  │  [10] SCL ──│─────────────│────────│──────│────│───│───┐   │    │
│  │             │             │        │      │    │   │   │   │    │
│  │  [2] DATA ──│─────────────│────────│──────│────│───│───│───│─┐ │    │
│  │             │             │        │      │    │   │   │   │ │ │    │
│  └─────────────┼─────────────┼────────┼──────┼────┼───┼───┼───┼─┼─┘    │
│                │             │        │      │    │   │   │   │ │      │
│   ┌────────────┴──┐     ┌────┴────────┴──┐   │   │   │   │   │ │      │
│   │  SSD1306      │     │  RYLR998       │   │   │   │   │   │ │      │
│   │  OLED         │     │  LoRa Module   │   │   │   │   │   │ │      │
│   ├───────────────┤     ├────────────────┤   │   │   │   │   │ │      │
│   │ VCC    3.3V   │     │ VDD    3.3V    │   │   │   │   │   │ │      │
│   │ GND    GND    │     │ GND    GND     │   │   │   │   │   │ │      │
│   │ SDA    [9]    │     │ RXD    [21]    │   │   │   │   │   │ │      │
│   │ SCL    [10]   │     │ TXD    [20]    │   │   │   │   │   │ │      │
│   └───────────────┘     └────────────────┘   │   │   │   │   │ │      │
│                                               │   │   │   │   │ │      │
│                        ┌──────────────────────┘   │   │   │   │ │      │
│   ┌────────────────────┴──────────────────────────┴───┴───┴───┘ │      │
│   │              WS2812B LED Strip (2 LEDs)                      │      │
│   │                                                               │      │
│   │  ┌──────────┐    ┌──────────┐                                │      │
│   │  │  LED 0   │    │  LED 1   │                                │      │
│   │  │  Water   │────│  Status  │                                │      │
│   │  └──────────┘    └──────────┘                                │      │
│   │                                                               │      │
│   │  VCC ─── 3.3V                                                │      │
│   │  GND ─── GND ────────────────────────────────────────────────┘      │
│   │  DIN ─── [2]                                                        │
│   └─────────────────────────────────────────────────────────────────────┘
│                                                                          │
│  [USB] ══════════════════════► 5V Power Supply                          │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

## Pin Definitions (config.h)

```cpp
// LoRa UART
#define LORA_RX_PIN     20
#define LORA_TX_PIN     21

// I2C (OLED Display)
#define I2C_SDA         9
#define I2C_SCL         10

// WS2812B LEDs
#define LED_DATA_PIN    2
#define NUM_LEDS        2

// LED Indices
#define LED_WATER       0     // Water level indicator
#define LED_STATUS      1     // System status indicator
```

## Features

### 1. Multi-Screen OLED Display
- **Screen 1 - Water Level**: Animated tank graphic with percentage
- **Screen 2 - Battery Status**: Transmitter battery voltage and percentage
- **Screen 3 - Signal Quality**: RSSI and SNR values
- **Screen 4 - Statistics**: Uptime, packets received, success rate

Auto-rotates every 8 seconds. Updates at 4 FPS for smooth animations.

### 2. Dual LED Status Indicators

#### LED 0 - Water Level
| Color | Water Level | Meaning |
|-------|-------------|---------|
| 🟢 Green | 75-100% | Tank full |
| 🔵 Cyan | 50-74% | Good level |
| 🟡 Yellow | 25-49% | Fair level |
| 🟠 Orange | 10-24% | Low - refill soon |
| 🔴 Red (blinking) | 0-9% | CRITICAL - refill now! |
| 🟠 Orange (blinking) | N/A | No data yet |

#### LED 1 - System Status
| Color | Pattern | Meaning |
|-------|---------|---------|
| 🔴 Red | Solid | LoRa hardware not connected |
| 🔵 Blue | Blinking | AP Mode - waiting for WiFi config |
| 🟢 Green | Solid | Connected - receiving fresh data |
| 🟡 Yellow | Solid | Data stale (>10 minutes old) |
| 🟡 Yellow | Blinking | No data for >15 minutes |
| 🔵 Cyan | Blinking | Waiting for first data |
| 🟠 Orange | Blinking | WiFi disconnected - reconnecting |

### 3. Web Dashboard

Access at `http://<receiver-ip>/`

**Features**:
- Real-time water level visualization
- Tank percentage with animated water graphic
- Battery voltage and percentage
- Signal strength (RSSI, SNR)
- Connection status with colored indicators
- Last update timestamp
- Auto-refresh every 2 seconds

**API Endpoints**:
- `GET /` - Main dashboard
- `GET /api/data` - JSON data endpoint
- `GET /config` - Configuration page
- `POST /saveWiFi` - Save WiFi credentials
- `POST /saveMqtt` - Save MQTT settings
- `POST /saveTank` - Save tank calibration
- `POST /saveLoRa` - Save LoRa settings

### 4. WiFi Management

#### Initial Setup (AP Mode)
On first boot or if WiFi credentials not saved:
1. Creates access point: **TankSync**
2. Connect to it (no password)
3. Navigate to `http://192.168.4.1/config`
4. Enter WiFi credentials
5. Device will restart and connect

#### Runtime Behavior
- Auto-reconnect if WiFi drops
- Retry interval: 5 minutes
- Falls back to AP mode if connection fails repeatedly
- Web server accessible in both modes

### 5. MQTT Publishing

Publishes to Home Assistant via MQTT:

**Topics**:
```
tank/water_level      - Water level in cm
tank/water_percent    - Water percentage (0-100)
tank/raw_distance     - Raw sensor reading
tank/battery_percent  - Transmitter battery %
tank/battery_voltage  - Transmitter battery voltage
tank/rssi            - LoRa signal strength (dBm)
tank/snr             - LoRa signal-to-noise ratio
tank/status          - Connection status
tank/packets_received - Total packets count
```

**Publishing Strategy**:
- Publishes **only when new data arrives** from transmitter
- NOT on a fixed interval (reduces MQTT traffic)
- Includes all sensor values in one burst

### 6. Watchdog Timer (WDT) Handling

**CRITICAL FIX**: Arduino framework already initializes WDT.
- We do NOT manually initialize WDT
- Strategy: `yield()` or `delay()` every ~20ms feeds watchdog
- Prevents WDT resets during long operations
- Extra delays after blocking operations (web server, MQTT)

## Configuration

### WiFi Settings
```cpp
// Saved in flash memory via web interface
String savedSSID;
String savedPassword;
```

### MQTT Settings
```cpp
#define DEFAULT_MQTT_SERVER     "192.168.0.163"
#define DEFAULT_MQTT_PORT       1885
#define DEFAULT_MQTT_USER       "mqtt-user"
#define DEFAULT_MQTT_PASSWORD   "techposts"
#define DEFAULT_MQTT_ENABLED    true
```

### Tank Calibration
```cpp
#define DEFAULT_MIN_DISTANCE    30      // cm - sensor reading when FULL
#define DEFAULT_MAX_DISTANCE    120     // cm - sensor reading when EMPTY
#define DEFAULT_TANK_CAPACITY   942.5   // Liters - total tank volume
```

**Calibration Procedure**:
1. Fill tank completely
2. Note sensor reading (e.g., 30cm) → `MIN_DISTANCE`
3. Empty tank completely
4. Note sensor reading (e.g., 120cm) → `MAX_DISTANCE`
5. Measure total tank capacity → `TANK_CAPACITY`
6. Update via web interface at `http://<ip>/config`

### LoRa Settings
```cpp
#define DEFAULT_LORA_FREQUENCY      "865000000"  // India: 865 MHz
#define DEFAULT_LORA_NETWORK_ID     6
#define DEFAULT_MY_ADDRESS          2
```

**Frequency Options**:
- India: 865000000 (865 MHz)
- Europe: 868000000 (868 MHz)
- US/Australia: 915000000 (915 MHz)

### Timeouts
```cpp
#define DATA_STALE_MS       600000  // 10 minutes
#define DATA_LOST_MS        900000  // 15 minutes
```

## Modular Code Structure

### config.h
- Pin definitions
- Hardware configuration
- Timeout values
- Default settings
- LED color definitions

### tank_data.h/cpp
- Tank data structure
- Water level calculations
- Data validation
- Settings persistence (NVS storage)

### lora_comm.h/cpp
- LoRa module initialization
- AT command sending
- Message parsing
- ACK transmission
- Connection state management

### display.h/cpp
- OLED initialization
- Screen rotation logic
- 4 display screens (water, battery, signal, stats)
- Status icons
- Tank graphic rendering

### wifi_manager.h/cpp
- WiFi connection management
- AP mode handling
- Credential persistence
- Auto-reconnect logic
- Connection status monitoring

### mqtt_handler.h/cpp
- MQTT connection
- Publishing sensor data
- Settings persistence
- Auto-reconnect logic
- Home Assistant integration

## Installation

### 1. Hardware Assembly
1. Wire all components according to pin diagram
2. Connect USB power
3. Verify all connections

### 2. Flash Firmware

**Required Libraries**:
```
- Adafruit GFX Library
- Adafruit SSD1306
- Adafruit NeoPixel
- PubSubClient
- Preferences (built-in)
```

**Board Settings**:
```
Board: ESP32C3 Dev Module
USB CDC On Boot: Enabled
Flash Size: 4MB
Partition Scheme: Default 4MB
Upload Speed: 921600
CPU Frequency: 160MHz
```

**Upload Process**:
1. Open `Receiver.ino` in Arduino IDE
2. Install all required libraries
3. Select correct board and port
4. Upload sketch
5. Open Serial Monitor (115200 baud)

### 3. Initial Configuration

**Method 1: Web Interface (Recommended)**
1. Device creates AP "TankSync" on first boot
2. Connect to "TankSync" WiFi
3. Navigate to `http://192.168.4.1/config`
4. Enter:
   - WiFi SSID and password
   - MQTT broker settings
   - Tank calibration values
   - LoRa settings (must match transmitter)
5. Click Save
6. Device restarts and connects to WiFi

**Method 2: Serial Monitor**
1. Connect via USB
2. Open Serial Monitor (115200 baud)
3. Note WiFi connection status
4. If connected, note IP address
5. Navigate to `http://<ip>/config`
6. Configure settings

### 4. Verify Operation

**Check LEDs**:
- LED 1 (Status): Should show connection state
- LED 0 (Water): Blinks orange until data received

**Check OLED Display**:
- Shows "READY" screen on boot
- Displays WiFi/LoRa status
- Rotates through 4 screens

**Check Web Dashboard**:
- Navigate to receiver IP address
- Should show current status (may show "Waiting for data" initially)
- Wait for transmitter to send data

**Check MQTT** (if enabled):
- Open Home Assistant
- Check MQTT integration
- Verify topics are being published
- Add sensors to dashboard

## Troubleshooting

### LoRa Not Working

**Problem**: Red LED, "LoRa ERROR" on display

**Solutions**:
- Check TX/RX wiring (pins may be crossed)
- Verify RYLR998 has 3.3V power
- Check antenna connection
- Ensure LoRa settings match transmitter
- Test with AT commands in Serial Monitor

### OLED Not Displaying

**Problem**: Blank screen

**Solutions**:
- Check I2C wiring (SDA = GPIO9, SCL = GPIO10)
- Verify 3.3V power to OLED
- Check I2C address (default 0x3C, some use 0x3D)
- Run I2C scanner sketch to detect address
- Try different OLED library version

### WiFi Won't Connect

**Problem**: Blue blinking LED, AP mode

**Solutions**:
- Verify SSID and password are correct
- Ensure router is 2.4GHz (ESP32 doesn't support 5GHz)
- Check WiFi signal strength
- Try moving closer to router
- Restart device and router
- Clear saved WiFi credentials via web interface

### MQTT Not Publishing

**Problem**: Data shows on display but not in Home Assistant

**Solutions**:
- Verify MQTT broker IP and port
- Check MQTT username/password
- Ensure MQTT is enabled in config
- Check MQTT broker logs
- Verify topics in configuration.yaml
- Restart MQTT broker
- Check Serial Monitor for MQTT connection status

### Watchdog Timer Resets

**Problem**: Device randomly reboots

**Solutions**:
- Already handled in code (yield() strategy)
- If still occurring, increase delays after blocking operations
- Check for infinite loops without yield()
- Verify web server isn't hanging
- Monitor Serial output for WDT warnings

### LEDs Not Lighting

**Problem**: No LED activity

**Solutions**:
- Check GPIO2 connection to DIN
- Verify 3.3V power to LED strip
- Ensure GND is connected
- Check LED_DATA_PIN in config.h
- Verify NUM_LEDS is set to 2
- Test with simple NeoPixel example

## Advanced Configuration

### Changing Display Update Rate

In `config.h`:
```cpp
#define SCREEN_ROTATE_MS    8000   // Screen rotation interval (ms)
#define DISPLAY_UPDATE_MS   250    // Display refresh rate (ms)
```

### Adjusting LED Brightness

In `Receiver.ino`:
```cpp
leds.setBrightness(75);  // 0-255, default 75
```

### Custom MQTT Topics

Edit in `mqtt_handler.cpp`:
```cpp
mqtt.publish("tank/water_level", String(tank.waterLevel).c_str());
// Change "tank/" prefix to your preference
```

### Debug Logging

In `config.h`:
```cpp
#define DEBUG_VERBOSE true  // Enable detailed debug logs
```

## Home Assistant Example

### configuration.yaml
```yaml
mqtt:
  sensor:
    - name: "Tank Water Level"
      state_topic: "tank/water_percent"
      unit_of_measurement: "%"
      icon: mdi:water-percent

    - name: "Tank Battery"
      state_topic: "tank/battery_percent"
      unit_of_measurement: "%"
      icon: mdi:battery
      device_class: battery
```

### Automation Example
```yaml
automation:
  - alias: "Tank Low Water Alert"
    trigger:
      - platform: numeric_state
        entity_id: sensor.tank_water_level
        below: 20
    action:
      - service: notify.mobile_app
        data:
          title: "Water Tank Alert"
          message: "Water level is low: {{ states('sensor.tank_water_level') }}%"
```

## API Reference

### GET /api/data

Returns JSON with current tank data:

```json
{
  "waterLevel": 85,
  "waterPercent": 72,
  "batteryPercent": 78,
  "batteryVoltage": 3.92,
  "rssi": -65,
  "snr": 9,
  "status": "Connected",
  "lastUpdate": "2024-01-15 14:32:10",
  "packetsReceived": 1234
}
```

## Performance

### Memory Usage
- Sketch: ~60% of program storage
- RAM: ~40% of dynamic memory
- Flash: Settings stored in NVS (non-volatile storage)

### Update Rates
- Display: 4 FPS (250ms)
- LED: 2 Hz (500ms)
- Web dashboard: Auto-refresh every 2s
- MQTT: Publishes on new data arrival

## License

MIT License - See main repository LICENSE file

## Support

For issues and questions, please open an issue on the main repository.

## Version History

### v2.0 - Modular Refactor
- Split code into logical modules
- Fixed watchdog timer issues
- Improved WiFi stability
- Added comprehensive web interface
- Better error handling
- Reduced to 2 LEDs (removed signal LED for simplicity)

### v1.0 - Initial Release
- Basic receiver functionality
- Web dashboard
- MQTT publishing
- OLED display
