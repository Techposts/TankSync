# 💧 LoRa Water Tank Level Monitor

A wireless water tank level monitoring system using LoRa (Long Range) radio communication with ESP32-C3 microcontrollers. Features real-time monitoring, Home Assistant integration, MQTT publishing, and a beautiful web dashboard with OLED display.

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Platform](https://img.shields.io/badge/platform-ESP32--C3-green.svg)
![LoRa](https://img.shields.io/badge/LoRa-RYLR998-orange.svg)

## 📋 Table of Contents

- [Features](#-features)
- [System Overview](#-system-overview)
- [Hardware Requirements](#-hardware-requirements)
- [Pin Configuration](#-pin-configuration)
- [Wiring Diagrams](#-wiring-diagrams)
- [Software Setup](#-software-setup)
- [Configuration](#-configuration)
- [LED Status Reference](#-led-status-reference)
- [Web Dashboard](#-web-dashboard)
- [Home Assistant Integration](#-home-assistant-integration)
- [Technical Specifications](#-technical-specifications)
- [Battery Life](#-battery-life)
- [Troubleshooting](#-troubleshooting)
- [Project Structure](#-project-structure)
- [Contributing](#-contributing)
- [License](#-license)

## ✨ Features

### Transmitter (Tank Side)
- **Ultra Low Power**: Deep sleep mode - 50+ days on 1000mAh battery
- **Solar Charging**: Compatible with TP4056 solar charging module
- **Waterproof Sensor**: AJ-SR04M ultrasonic sensor (IP67)
- **Visual Status**: WS2812B RGB LED with intelligent power control
- **Battery Monitoring**: Voltage divider with accurate percentage calculation
- **Reliable Communication**: ACK-based transmission with retry logic
- **Long Range**: Up to 10+ km line-of-sight using LoRa

### Receiver (Indoor)
- **Modular Architecture**: Clean, maintainable code structure
- **Multi-Screen OLED**: 4 rotating screens showing all data
- **Dual LED Status**: Separate indicators for water level and system status
- **Web Dashboard**: Beautiful real-time UI with auto-refresh
- **MQTT Publishing**: Full Home Assistant integration
- **WiFi Management**: Web-based configuration with AP fallback
- **Data Persistence**: Settings saved in flash memory
- **Responsive Design**: Mobile-friendly web interface

## 🏗 System Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              TRANSMITTER                                     │
│                         (On Tank - Battery Powered)                          │
│                                                                              │
│  ┌─────────────┐     ┌─────────────┐     ┌─────────────┐                    │
│  │ AJ-SR04M    │     │ ESP32-C3    │     │  RYLR998    │                    │
│  │ Ultrasonic  ├────►│ SuperMini   ├────►│   LoRa      │ )))               │
│  │ (IP67)      │     │ (Deep Sleep)│     │  868/915MHz │                    │
│  └─────────────┘     └──────┬──────┘     └─────────────┘                    │
│                             │                                                │
│  ┌─────────────┐     ┌──────┴──────┐     ┌─────────────┐                    │
│  │  WS2812B    │     │ 18650 Li-ion│     │  TP4056     │                    │
│  │  1x LED     │     │   Battery   │     │  + Solar    │                    │
│  │ (GPIO PWR)  │     │  (3.7V)     │     │  Charging   │                    │
│  └─────────────┘     └─────────────┘     └─────────────┘                    │
│                                                                              │
│  Deep Sleep: ~10µA  |  Active: ~80mA  |  Battery Life: 50+ days             │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ LoRa Radio (868/915 MHz)
                                    │ Range: Up to 10+ km LOS
                                    │ Protocol: AT Commands
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                           RECEIVER (Modular)                                 │
│                          (Indoor - USB Powered)                              │
│                                                                              │
│  ┌─────────────┐     ┌─────────────┐     ┌─────────────┐                    │
│  │  RYLR998    │     │ ESP32-C3    │     │ SSD1306     │                    │
│  │   LoRa      ├────►│ SuperMini   ├────►│ OLED 128x64 │                    │
│  │  868/915MHz │     │ (WiFi ON)   │     │   (I2C)     │                    │
│  └─────────────┘     └──────┬──────┘     └─────────────┘                    │
│                             │                                                │
│                             │            ┌─────────────┐                     │
│                             ├───────────►│  WS2812B    │                     │
│                             │            │  2x LEDs    │                     │
│                             │            │  Status     │                     │
│                             │            └─────────────┘                     │
│                             │                                                │
│                             ├────► WiFi → Web Dashboard (Port 80)            │
│                             └────► WiFi → MQTT → Home Assistant              │
│                                                                              │
│  Modules: WiFi Manager | MQTT Handler | Display | LoRa Comm | Tank Data     │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 🛠 Hardware Requirements

### Transmitter (Tank Side)

| Component | Quantity | Notes | Approx. Cost |
|-----------|----------|-------|--------------|
| ESP32-C3 SuperMini | 1 | Main controller | $3-5 |
| RYLR998 LoRa Module | 1 | 868/915 MHz | $8-12 |
| AJ-SR04M Ultrasonic Sensor | 1 | Waterproof (IP67) | $3-5 |
| WS2812B LED | 1 LED | Status indicator | $1 |
| 10KΩ Resistors | 2 | Voltage divider | $0.20 |
| 18650 Li-ion Battery | 1 | 1000mAh+ recommended | $3-5 |
| 18650 Battery Holder | 1 | With wires | $0.50 |
| TP4056 Charging Module | 1 | Optional, for solar | $0.50 |
| Solar Panel | 1 | 5V 100mA (optional) | $3-5 |
| Waterproof Enclosure | 1 | IP65 or better | $5-10 |
| Antenna | 1 | 868/915 MHz (comes with RYLR998) | Included |

**Total Cost**: ~$30-50 (without solar)

### Receiver (Indoor)

| Component | Quantity | Notes | Approx. Cost |
|-----------|----------|-------|--------------|
| ESP32-C3 SuperMini | 1 | Main controller | $3-5 |
| RYLR998 LoRa Module | 1 | 868/915 MHz | $8-12 |
| SSD1306 OLED Display | 1 | 128x64, I2C, 0.96" | $3-5 |
| WS2812B LED Strip | 2 LEDs | Status indicators | $1 |
| USB Cable | 1 | Power supply (5V) | $2 |
| Enclosure | 1 | Optional | $3-5 |
| Antenna | 1 | 868/915 MHz (comes with RYLR998) | Included |

**Total Cost**: ~$20-30

### Tools Required

- Soldering iron & solder
- Wire strippers
- Multimeter
- Jumper wires (DuPont connectors)
- Heat shrink tubing (optional)
- Hot glue gun (for mounting)

## 🔌 Pin Configuration

### Transmitter Pin Mapping

| ESP32-C3 Pin | Component | Function | Notes |
|--------------|-----------|----------|-------|
| **GPIO21** | RYLR998 RXD | UART TX | ESP TX → LoRa RX |
| **GPIO20** | RYLR998 TXD | UART RX | ESP RX ← LoRa TX |
| **GPIO4** | AJ-SR04M TRIG | Trigger | Ultrasonic trigger pulse |
| **GPIO5** | AJ-SR04M ECHO | Echo | Ultrasonic echo input |
| **GPIO3** | Voltage Divider | ADC | Battery voltage sensing |
| **GPIO7** | WS2812B VCC | Power | LED power control |
| **GPIO2** | WS2812B DIN | Data | LED data |
| **3.3V** | Components | Power | LoRa, Sensor power |
| **5V** | Battery+ | Battery | Via TP4056 |
| **GND** | Common | Ground | All components |

**CRITICAL**: GPIO7 controls LED power for zero sleep current!

### Receiver Pin Mapping

| ESP32-C3 Pin | Component | Function | Notes |
|--------------|-----------|----------|-------|
| **GPIO21** | RYLR998 RXD | UART TX | ESP TX → LoRa RX |
| **GPIO20** | RYLR998 TXD | UART RX | ESP RX ← LoRa TX |
| **GPIO9** | SSD1306 SDA | I2C Data | OLED display data |
| **GPIO10** | SSD1306 SCL | I2C Clock | OLED display clock |
| **GPIO2** | WS2812B DIN | Data | LED strip data |
| **3.3V** | Components | Power | LoRa, OLED, LEDs |
| **USB** | Power | 5V | Always powered |
| **GND** | Common | Ground | All components |

## 📐 Wiring Diagrams

### Transmitter Wiring (Detailed)

```
╔═══════════════════════════════════════════════════════════════════════════╗
║                         TRANSMITTER CONNECTIONS                            ║
╚═══════════════════════════════════════════════════════════════════════════╝

┌──────────────────────────────────────────────────────────────────────────┐
│                      ESP32-C3 SuperMini                                   │
│                                                                           │
│    [21] TX ────────────────────────────┐                                 │
│    [20] RX ─────────────────┐          │                                 │
│    [4]  TRIG ────────┐      │          │                                 │
│    [5]  ECHO ─────┐  │      │          │                                 │
│    [7]  LED_PWR ──│──│──────│──────────│──────┐                          │
│    [2]  LED_DAT ──│──│──────│──────────│──────│────┐                     │
│    [3]  ADC ──────│──│──────│──────────│──────│────│──┐                  │
│                   │  │      │          │      │    │  │                  │
│    3.3V ──────┬───│──│──────│──────────│──────│────│──│──────┐           │
│    GND ───────│───│──│──────│──────────│──────│────│──│──────│──┐        │
│    5V ────────│───│──│──────│──────────│──────│────│──│──────│──│──┐     │
└───────────────┼───┼──┼──────┼──────────┼──────┼────┼──┼──────┼──┼──┼─────┘
                │   │  │      │          │      │    │  │      │  │  │
   ┌────────────┘   │  │      │          │      │    │  │      │  │  │
   │  ┌─────────────┘  │      │          │      │    │  │      │  │  │
   │  │  ┌─────────────┘      │          │      │    │  │      │  │  │
   │  │  │                    │          │      │    │  │      │  │  │
   ▼  ▼  ▼                    ▼          ▼      ▼    ▼  ▼      ▼  ▼  ▼
┌──────────────┐       ┌───────────┐  ┌─────────────────┐  ┌────────────┐
│  AJ-SR04M    │       │ RYLR998   │  │   WS2812B       │  │  BATTERY   │
│  Ultrasonic  │       │ LoRa      │  │   (1 LED)       │  │  SYSTEM    │
├──────────────┤       ├───────────┤  ├─────────────────┤  ├────────────┤
│ VCC    [3.3V]│       │VDD [3.3V] │  │ VCC      [GPIO7]│  │            │
│ GND    [GND] │       │GND [GND]  │  │ GND      [GND]  │  │ 18650 Cell │
│ TRIG   [4]   │       │RXD [21]   │  │ DIN      [2]    │  │   3.7V     │
│ ECHO   [5]   │       │TXD [20]   │  └─────────────────┘  │ 1000-3000  │
└──────────────┘       │ANT (ext.) │                       │    mAh     │
                       └───────────┘                       │            │
                                                           │ (+)     (-)│
                 ┌──────────────────────────────────────────┴─┐     ┌──┘
                 │                                              │     │
            ┌────┴────┐                                         │     │
            │ TP4056  │  ← Optional Solar Charging             │     │
            │ Charge  │                                         │     │
            │ Module  │                                         │     │
            ├─────────┤                                         │     │
            │ B+   B- │─────────────────────────────────────────┘     │
            │ OUT+ OUT│──────────────────────────────────┐            │
            │ IN+  IN-│← Solar Panel (5V)                │            │
            └─────────┘                                  │            │
                                                         ▼            ▼
                                       ┌─────────────────────────────────┐
                                       │  Voltage Divider (to GPIO3)     │
                                       │                                 │
                                       │  OUT+ ─── 10KΩ ───┬─── 10KΩ ───│─ GND
                                       │                    │            │
                                       │                 GPIO3 (ADC)     │
                                       │                                 │
                                       │  Measures: 0-4.2V → 0-2.1V     │
                                       └─────────────────────────────────┘
```

### RYLR998 Module Pinout

```
Looking at module from TOP (antenna side up):

    ┌─────────────────────┐
    │     RYLR998         │
    │   LoRa Module       │
    │                     │
    │  ╔═══════════════╗  │
    │  ║   ANTENNA     ║  │ ← External antenna connector
    │  ╚═══════════════╝  │
    │                     │
    │  Pin Layout:        │
    │  ┌────────────────┐ │
    │  │ 1  VDD  (3.3V) │ │ ← Power (2.3V - 3.6V)
    │  │ 2  NC          │ │
    │  │ 3  RXD         │ │ ← UART RX (from ESP32 TX)
    │  │ 4  TXD         │ │ ← UART TX (to ESP32 RX)
    │  │ 5  GND         │ │ ← Ground
    │  └────────────────┘ │
    └─────────────────────┘
```

### Receiver Wiring (Detailed)

```
╔═══════════════════════════════════════════════════════════════════════════╗
║                          RECEIVER CONNECTIONS                              ║
╚═══════════════════════════════════════════════════════════════════════════╝

┌──────────────────────────────────────────────────────────────────────────┐
│                      ESP32-C3 SuperMini                                   │
│                                                                           │
│    [21] TX ────────────────────┐                                         │
│    [20] RX ─────────────┐      │                                         │
│    [9]  SDA ─────┐      │      │                                         │
│    [10] SCL ──┐  │      │      │                                         │
│    [2]  DATA ─│──│──────│──────│────┐                                    │
│               │  │      │      │    │                                    │
│    3.3V ──┬───│──│──────│──────│────│─────┐                              │
│    GND ───│───│──│──────│──────│────│─────│────┐                         │
│    USB ═══╪═══╪══╪══════╪══════╪════╪═════╪════╪══ 5V Power              │
└───────────┼───┼──┼──────┼──────┼────┼─────┼────┼─────────────────────────┘
            │   │  │      │      │    │     │    │
            │   │  │      │      │    │     │    │
            ▼   ▼  ▼      ▼      ▼    ▼     ▼    ▼
         ┌──────────┐  ┌────────────┐  ┌──────────────────┐
         │ SSD1306  │  │ RYLR998    │  │   WS2812B        │
         │  OLED    │  │ LoRa       │  │   (2 LEDs)       │
         │ 128x64   │  │            │  │                  │
         │  (I2C)   │  │            │  │  LED0: Water     │
         ├──────────┤  ├────────────┤  │  LED1: Status    │
         │VCC [3.3V]│  │VDD  [3.3V] │  ├──────────────────┤
         │GND [GND] │  │GND  [GND]  │  │ VCC     [3.3V]   │
         │SDA [9]   │  │RXD  [21]   │  │ GND     [GND]    │
         │SCL [10]  │  │TXD  [20]   │  │ DIN     [2]      │
         └──────────┘  │ANT  (ext.) │  └──────────────────┘
                       └────────────┘
```

### Tank Sensor Placement

```
     Mounting Bracket
           │
           ▼
     ┌─────────────────────┐
     │  AJ-SR04M Sensor    │ ← Mount ABOVE water, facing DOWN
     │  [T] [R] [T] [R]    │    (Waterproof, can be exposed)
     └──────────┬──────────┘
                │ Cable to ESP32
                │
                │ 25-30cm  ← SENSOR_OFFSET (full tank reading)
                │            Minimum safe distance: 20cm
     ═══════════╧═══════════════ ← FULL TANK (100%)
                │                  Reading: 25-30cm
                │
                │
                │ 90-120cm ← TANK_DEPTH
                │            Actual water column
                │
                │
     ══════════════════════════ ← EMPTY TANK (0%)
                                  Reading: 120-150cm

Calculation:
  Water Level (cm) = SENSOR_READING - SENSOR_OFFSET
  Water % = 100 - ((READING - MIN_DIST) / (MAX_DIST - MIN_DIST)) × 100

Example:
  Sensor at 30cm (full): Water = 100%
  Sensor at 75cm: Water = 50%
  Sensor at 120cm (empty): Water = 0%
```

## 💻 Software Setup

### Arduino IDE Configuration

#### 1. Install Arduino IDE

Download Arduino IDE 2.0+ from [arduino.cc](https://www.arduino.cc/en/software)

#### 2. Add ESP32 Board Support

1. Open Arduino IDE
2. Go to `File` → `Preferences`
3. Add to "Additional Board Manager URLs":
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
4. Go to `Tools` → `Board` → `Boards Manager`
5. Search "esp32"
6. Install "ESP32 by Espressif Systems" (version 2.0.0+)

#### 3. Install Required Libraries

Go to `Sketch` → `Include Library` → `Manage Libraries` and install:

**For Transmitter**:
- `Adafruit NeoPixel` by Adafruit

**For Receiver**:
- `Adafruit GFX Library` by Adafruit
- `Adafruit SSD1306` by Adafruit
- `Adafruit NeoPixel` by Adafruit
- `PubSubClient` by Nick O'Leary
- `Preferences` (built-in, no installation needed)

#### 4. Board Settings

Select the following in `Tools` menu:

| Setting | Value |
|---------|-------|
| **Board** | ESP32C3 Dev Module |
| **USB CDC On Boot** | Enabled |
| **CPU Frequency** | 160MHz |
| **Flash Size** | 4MB (32Mb) |
| **Flash Mode** | QIO |
| **Partition Scheme** | Default 4MB with spiffs |
| **Upload Speed** | 921600 |
| **Core Debug Level** | None (or Info for debugging) |

## ⚙️ Configuration

### Transmitter Configuration

Edit in `Transmitter/Transmitter.ino`:

```cpp
// Sleep Configuration
#define SLEEP_MINUTES       5                 // How often to wake and transmit

// LoRa Configuration
#define LORA_FREQUENCY      "865000000"       // India: 865 MHz
                                              // EU: 868000000
                                              // US/AU: 915000000
#define LORA_NETWORK_ID     6                 // Must match receiver (3-15 or 18)
#define LORA_TX_POWER       14                // TX power in dBm (0-22)
#define MY_ADDRESS          1                 // This transmitter's address
#define RECEIVER_ADDRESS    2                 // Receiver's address

// Battery Configuration
#define VOLTAGE_DIVIDER_RATIO   2.0           // (R1 + R2) / R2 = (10K + 10K) / 10K
#define ADC_CALIBRATION_FACTOR  0.918         // Adjust if voltage reading is off
#define BATTERY_FULL_VOLTAGE    4.2           // Li-ion fully charged
#define BATTERY_EMPTY_VOLTAGE   3.0           // Safe discharge cutoff

// Sensor Configuration
#define SENSOR_NUM_READINGS     7             // Number of readings to average
#define SENSOR_READING_INTERVAL 120           // ms between readings (min 100ms)
#define DISTANCE_OFFSET_CM      0             // Calibration offset
```

### Receiver Configuration

**Method 1: Web Interface** (Recommended)

1. On first boot, device creates WiFi AP "TankSync"
2. Connect to "TankSync"
3. Navigate to `http://192.168.4.1/config`
4. Configure:
   - **WiFi**: SSID and password
   - **MQTT**: Broker IP, port, username, password
   - **Tank**: Min distance, max distance, capacity
   - **LoRa**: Frequency, network ID, address (must match transmitter)
5. Click Save - device will restart

**Method 2: Edit config.h** (Advanced)

Edit `ReceiverModular/config.h`:

```cpp
// MQTT Configuration
#define DEFAULT_MQTT_SERVER     "192.168.0.163"  // Your MQTT broker IP
#define DEFAULT_MQTT_PORT       1885
#define DEFAULT_MQTT_USER       "mqtt-user"
#define DEFAULT_MQTT_PASSWORD   "your-password"
#define DEFAULT_MQTT_ENABLED    true

// Tank Configuration
#define DEFAULT_MIN_DISTANCE    30      // cm - sensor reading when FULL
#define DEFAULT_MAX_DISTANCE    120     // cm - sensor reading when EMPTY
#define DEFAULT_TANK_CAPACITY   942.5   // Liters

// LoRa Configuration
#define DEFAULT_LORA_FREQUENCY      "865000000"  // Must match transmitter
#define DEFAULT_LORA_NETWORK_ID     6            // Must match transmitter
#define DEFAULT_MY_ADDRESS          2            // Receiver address
```

## 💡 LED Status Reference

### Transmitter LED (Single LED)

| Color | Duration | Meaning |
|-------|----------|---------|
| 🟡 **Yellow** | Solid | Starting up / Initializing |
| 🔵 **Blue** | Solid | Taking sensor measurements |
| 🔵 **Cyan** | Solid | Transmitting data via LoRa |
| 🟣 **Magenta** | Solid | Waiting for ACK from receiver |
| 🟠 **Orange** | Solid | Retrying transmission |
| 🟢 **Green** | 3 blinks | ✓ Transmission successful! |
| 🔴 **Red** | 10 blinks | ✗ LoRa initialization failed |
| 🔴 **Red** | 5 blinks | ✗ Transmission failed (no ACK) |

### Receiver LEDs (2 LEDs)

#### LED 0 - Water Level Status

| Color | Pattern | Water Level | Action Required |
|-------|---------|-------------|-----------------|
| 🟢 **Green** | Solid | 75-100% | Tank is full |
| 🔵 **Cyan** | Solid | 50-74% | Good level |
| 🟡 **Yellow** | Solid | 25-49% | Fair - monitor |
| 🟠 **Orange** | Solid | 10-24% | Low - refill soon |
| 🔴 **Red** | Blinking | 0-9% | CRITICAL - refill now! |
| 🟠 **Orange** | Blinking | N/A | No data received yet |

#### LED 1 - System Status

| Color | Pattern | Meaning | Action Required |
|-------|---------|---------|-----------------|
| 🔴 **Red** | Solid | LoRa hardware not connected | Check wiring |
| 🔵 **Blue** | Blinking | AP Mode - no WiFi config | Configure WiFi |
| 🟢 **Green** | Solid | Connected - fresh data | All OK |
| 🟡 **Yellow** | Solid | Data stale (>10 min) | Check transmitter |
| 🟡 **Yellow** | Blinking | No data for >15 min | Transmitter offline? |
| 🔵 **Cyan** | Blinking | Waiting for first data | Normal on startup |
| 🟠 **Orange** | Blinking | WiFi disconnected | Check router |

## 🌐 Web Dashboard

### Accessing the Dashboard

1. **Find Receiver IP**:
   - Check OLED display (shows IP on status screens)
   - Check serial monitor
   - Check router's DHCP client list
   - If in AP mode: `http://192.168.4.1`

2. **Open in Browser**:
   ```
   http://<receiver-ip>/
   ```

### Dashboard Features

- **Real-time Updates**: Auto-refreshes every 2 seconds
- **Visual Tank**: Animated water level with wave effect
- **Key Metrics**:
  - Water level (cm and %)
  - Battery status (V and %)
  - Signal strength (RSSI, SNR)
  - Connection status
  - Last update timestamp
- **LED Simulation**: Shows current LED colors matching physical LEDs
- **Responsive Design**: Works on mobile devices

### Configuration Page

Access configuration at:
```
http://<receiver-ip>/config
```

**Settings Available**:
1. **WiFi Configuration**
   - SSID
   - Password
   - Auto-connect on boot

2. **MQTT Settings**
   - Broker IP address
   - Port (default 1885)
   - Username
   - Password
   - Enable/disable MQTT

3. **Tank Calibration**
   - Min distance (sensor reading when full)
   - Max distance (sensor reading when empty)
   - Tank capacity (liters)

4. **LoRa Settings**
   - Frequency (must match transmitter)
   - Network ID (must match transmitter)
   - Device address

5. **Alert Settings** (Future feature)
   - Low water threshold
   - Low battery threshold
   - Email notifications

## 🏠 Home Assistant Integration

### MQTT Configuration

Add to your Home Assistant `configuration.yaml`:

```yaml
mqtt:
  sensor:
    # Water Level
    - name: "Tank Water Level"
      state_topic: "tank/water_level"
      unit_of_measurement: "cm"
      icon: mdi:water
      device_class: distance

    - name: "Tank Water Percent"
      state_topic: "tank/water_percent"
      unit_of_measurement: "%"
      icon: mdi:water-percent

    - name: "Tank Raw Distance"
      state_topic: "tank/raw_distance"
      unit_of_measurement: "cm"
      icon: mdi:ruler

    # Battery
    - name: "Tank Battery Percent"
      state_topic: "tank/battery_percent"
      unit_of_measurement: "%"
      icon: mdi:battery
      device_class: battery

    - name: "Tank Battery Voltage"
      state_topic: "tank/battery_voltage"
      unit_of_measurement: "V"
      icon: mdi:flash
      device_class: voltage

    # Signal Quality
    - name: "Tank Signal RSSI"
      state_topic: "tank/rssi"
      unit_of_measurement: "dBm"
      icon: mdi:signal
      device_class: signal_strength

    - name: "Tank Signal SNR"
      state_topic: "tank/snr"
      unit_of_measurement: "dB"
      icon: mdi:signal-variant

    # System Stats
    - name: "Tank Connection Status"
      state_topic: "tank/status"
      icon: mdi:connection

    - name: "Tank Packets Received"
      state_topic: "tank/packets_received"
      icon: mdi:counter
```

### Lovelace Dashboard Card

```yaml
type: vertical-stack
title: Water Tank Monitor
cards:
  # Water Level Card
  - type: gauge
    entity: sensor.tank_water_percent
    name: Water Level
    min: 0
    max: 100
    severity:
      green: 50
      yellow: 25
      red: 0

  # Details Card
  - type: entities
    entities:
      - entity: sensor.tank_water_level
        name: Water Level
      - entity: sensor.tank_battery_percent
        name: Battery
      - entity: sensor.tank_signal_rssi
        name: Signal Strength
      - entity: sensor.tank_connection_status
        name: Status
      - entity: sensor.tank_packets_received
        name: Packets

  # Battery Card
  - type: gauge
    entity: sensor.tank_battery_percent
    name: Transmitter Battery
    min: 0
    max: 100
    severity:
      green: 50
      yellow: 20
      red: 0
```

### Automation Examples

#### Low Water Alert

```yaml
automation:
  - alias: "Tank Low Water Alert"
    trigger:
      - platform: numeric_state
        entity_id: sensor.tank_water_percent
        below: 20
    action:
      - service: notify.mobile_app
        data:
          title: "💧 Water Tank Alert"
          message: "Water level is low: {{ states('sensor.tank_water_percent') }}%"
          data:
            priority: high
```

#### Battery Low Alert

```yaml
automation:
  - alias: "Tank Battery Low Alert"
    trigger:
      - platform: numeric_state
        entity_id: sensor.tank_battery_percent
        below: 20
    condition:
      - condition: time
        after: "09:00:00"
        before: "21:00:00"  # Only during daytime
    action:
      - service: notify.mobile_app
        data:
          title: "🔋 Tank Sensor Battery"
          message: "Battery is low: {{ states('sensor.tank_battery_percent') }}%. Consider solar charging."
```

#### Connection Lost Alert

```yaml
automation:
  - alias: "Tank Connection Lost"
    trigger:
      - platform: state
        entity_id: sensor.tank_connection_status
        to: "Lost"
        for:
          minutes: 30
    action:
      - service: notify.mobile_app
        data:
          title: "📡 Tank Monitor Offline"
          message: "No data received for 30+ minutes. Check transmitter."
```

## 📊 Technical Specifications

### Communication Protocol

**Message Format** (Transmitter → Receiver):
```
TANK:<distance>:<battery%>:<voltage>:<msgId>
```

Example:
```
TANK:85:78:3.92:1704123456
```

**ACK Format** (Receiver → Transmitter):
```
ACK:<msgId>
```

### LoRa Parameters (RYLR998)

| Parameter | Value | Notes |
|-----------|-------|-------|
| Frequency | 820-960 MHz | Region dependent |
| TX Power | 0-22 dBm | Configurable |
| Sensitivity | -148 dBm | |
| Range | Up to 10 km | Line of sight |
| Interface | UART | AT commands, 115200 baud |
| Sleep Current | 10 µA | |
| RX Current | 17.5 mA | |
| TX Current | 140 mA @ 22 dBm | |

### AJ-SR04M Ultrasonic Sensor

| Parameter | Value |
|-----------|-------|
| Operating Voltage | 3.3V - 5V |
| Operating Current | 30 mA |
| Range | 20 cm - 600 cm |
| Accuracy | ±1 cm |
| Beam Angle | 75° |
| Waterproof Rating | IP67 |
| Measurement Interval | Minimum 100ms |

### ESP32-C3 SuperMini

| Parameter | Value |
|-----------|-------|
| CPU | RISC-V 32-bit @ 160MHz |
| RAM | 400 KB SRAM |
| Flash | 4 MB |
| WiFi | 2.4 GHz 802.11 b/g/n |
| Bluetooth | BLE 5.0 |
| ADC | 12-bit, 6 channels |
| Deep Sleep Current | ~10 µA |
| Operating Voltage | 3.3V |
| GPIO | 22 pins |

## 🔋 Battery Life Calculation

### Power Consumption Analysis

#### Transmitter (5-minute wake cycle)

| State | Current | Duration | Energy |
|-------|---------|----------|--------|
| Deep Sleep | 10 µA | 297 sec | 0.00083 mAh |
| LoRa Init | 80 mA | 1 sec | 0.022 mAh |
| Sensor Reading | 30 mA | 1 sec | 0.008 mAh |
| LoRa TX + ACK | 140 mA | 1 sec | 0.038 mAh |
| **Total per cycle** | - | **300 sec** | **~0.068 mAh** |

**Daily Consumption**:
```
Cycles per day: 24 hours × 60 min / 5 min = 288 cycles
Daily usage: 288 × 0.068 mAh = 19.6 mAh/day
```

**Battery Life**:
```
1000 mAh battery: 1000 / 19.6 = ~51 days
2000 mAh battery: 2000 / 19.6 = ~102 days
3000 mAh battery: 3000 / 19.6 = ~153 days
```

### Extending Battery Life

1. **Increase sleep interval**: 10 minutes → 102 days, 15 minutes → 153 days
2. **Reduce TX power**: 14 dBm → 10 dBm (if signal allows)
3. **Solar charging**: 5V 100mA panel maintains charge indefinitely
4. **Larger battery**: 18650 cells available up to 3500 mAh

## 🔧 Troubleshooting

### LoRa Communication Issues

| Problem | Possible Cause | Solution |
|---------|----------------|----------|
| No +OK response | Wiring incorrect | Check TX/RX are crossed: ESP TX→LoRa RX |
| +ERR=1 | Missing line ending | Use `println()` not `print()` |
| No data received | Mismatch settings | Verify BAND and NETWORKID match on both |
| Weak signal | Antenna issue | Check antenna connection, try repositioning |
| Intermittent | Power supply | Ensure stable 3.3V, check battery voltage |

### Sensor Issues

| Problem | Possible Cause | Solution |
|---------|----------------|----------|
| Distance = -1 | No echo received | Check wiring, verify 3.3V power |
| Erratic readings | Angle/vibration | Mount sensor perpendicular, secure firmly |
| Always max reading | Too close | Minimum distance 20cm |
| Timeout | Sensor damaged | Test with multimeter, replace if needed |
| Unstable values | Interference | Add delay between readings, use median |

### Display Issues

| Problem | Possible Cause | Solution |
|---------|----------------|----------|
| Blank screen | I2C address wrong | Try 0x3C or 0x3D, run I2C scanner |
| Garbled display | Loose connection | Check SDA/SCL wiring |
| No response | Power issue | Verify 3.3V supply |
| Dim display | Contrast setting | Check OLED library contrast value |

### WiFi Issues

| Problem | Possible Cause | Solution |
|---------|----------------|----------|
| Won't connect | Wrong credentials | Clear saved WiFi, reconfigure |
| Connects then drops | Signal strength | Move closer to router |
| AP mode stuck | No 2.4GHz WiFi | ESP32 doesn't support 5GHz |
| IP not shown | DHCP issue | Check router DHCP pool |

### Web Dashboard Issues

| Problem | Possible Cause | Solution |
|---------|----------------|----------|
| Can't access | Wrong IP | Check serial monitor or OLED for IP |
| Slow loading | WiFi signal | Improve signal strength |
| Not updating | JS error | Clear browser cache, try different browser |
| Config won't save | Flash full | Check available flash space |

## 📁 Project Structure

```
LoRa-Water-Tank-Monitor/
├── README.md                          # This file
├── LICENSE                            # MIT License
│
├── Transmitter/                       # Battery-powered tank unit
│   ├── Transmitter.ino               # Main sketch
│   └── README.md                     # Transmitter documentation
│
├── ReceiverModular/                   # USB-powered indoor unit (UPDATED)
│   ├── Receiver.ino                  # Main program loop
│   ├── config.h                      # Configuration & pin definitions
│   ├── tank_data.h                   # Tank data management header
│   ├── tank_data.cpp                 # Tank data implementation
│   ├── lora_comm.h                   # LoRa communication header
│   ├── lora_comm.cpp                 # LoRa communication implementation
│   ├── display.h                     # OLED display header
│   ├── display.cpp                   # OLED display implementation
│   ├── wifi_manager.h                # WiFi management header
│   ├── wifi_manager.cpp              # WiFi management implementation
│   ├── mqtt_handler.h                # MQTT handler header
│   ├── mqtt_handler.cpp              # MQTT handler implementation
│   └── README.md                     # Receiver documentation
│
├── docs/                              # Documentation
│   ├── images/                       # Circuit diagrams, photos
│   │   ├── system-diagram.png
│   │   ├── transmitter-wiring.png
│   │   └── receiver-wiring.png
│   ├── CALIBRATION.md                # Calibration guide
│   └── RYLR998-AT-Commands.md       # LoRa module reference
│
└── hardware/                          # Hardware files
    ├── BOM.csv                       # Bill of materials
    ├── enclosure-notes.md            # Enclosure recommendations
    └── pcb/                          # Optional PCB designs
```

## 🤝 Contributing

Contributions are welcome! Here's how you can help:

1. **Report Bugs**: Open an issue with detailed description
2. **Suggest Features**: Describe your idea in an issue
3. **Submit Pull Requests**:
   - Fork the repository
   - Create feature branch (`git checkout -b feature/AmazingFeature`)
   - Commit changes (`git commit -m 'Add AmazingFeature'`)
   - Push to branch (`git push origin feature/AmazingFeature`)
   - Open Pull Request

### Development Guidelines

- Follow existing code style
- Comment complex logic
- Test on actual hardware before PR
- Update documentation for new features
- Keep commits atomic and descriptive

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

**Summary**: Free to use, modify, and distribute with attribution.

## 🙏 Acknowledgments

- **Hardware**:
  - [REYAX](https://reyax.com/) for RYLR998 LoRa module
  - [Espressif](https://www.espressif.com/) for ESP32-C3
  - [Adafruit](https://www.adafruit.com/) for excellent libraries

- **Software**:
  - Arduino community for ESP32 support
  - [PubSubClient](https://github.com/knolleary/pubsubclient) MQTT library
  - [Home Assistant](https://www.home-assistant.io/) community

- **Inspiration**:
  - DIY home automation community
  - LoRa enthusiasts worldwide

## 📧 Support

Need help? Here's how to get support:

1. **Check Documentation**: Read this README and module-specific READMEs
2. **Search Issues**: Check if someone else had the same problem
3. **Open an Issue**: Provide detailed information:
   - Hardware setup
   - Software version
   - Serial monitor output
   - Photos of connections
4. **Discussions**: Join [Discussions](../../discussions) for questions

## 🌟 Show Your Support

If this project helped you, please:
- ⭐ Star this repository
- 📢 Share with others
- 📝 Write a blog post about your build
- 📷 Share photos of your installation

---

**Happy Monitoring! 💧**

Made with ❤️ for the Home Automation Community

Last Updated: January 2025
