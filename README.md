# 💧 LoRa Water Tank Level Monitor

A wireless water tank level monitoring system using LoRa (Long Range) radio communication with ESP32-C3 microcontrollers. Features real-time monitoring, Home Assistant integration, and a beautiful web dashboard.

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Platform](https://img.shields.io/badge/platform-ESP32--C3-green.svg)
![LoRa](https://img.shields.io/badge/LoRa-RYLR998-orange.svg)

## 📋 Table of Contents

- [Features](#-features)
- [System Overview](#-system-overview)
- [Hardware Requirements](#-hardware-requirements)
- [Wiring Diagrams](#-wiring-diagrams)
- [Software Setup](#-software-setup)
- [Configuration](#-configuration)
- [Installation](#-installation)
- [LED Status Reference](#-led-status-reference)
- [Web Dashboard](#-web-dashboard)
- [Home Assistant Integration](#-home-assistant-integration)
- [Technical Specifications](#-technical-specifications)
- [Battery Life](#-battery-life)
- [Troubleshooting](#-troubleshooting)
- [Contributing](#-contributing)
- [License](#-license)

## ✨ Features

- **Long Range Communication**: Up to 10+ km line-of-sight using LoRa
- **Ultra Low Power**: Deep sleep mode for months of battery life
- **Real-time Monitoring**: Water level, battery status, signal strength
- **Visual Indicators**: WS2812B RGB LEDs for instant status
- **Web Dashboard**: Beautiful real-time UI with auto-refresh
- **Home Assistant**: Full MQTT integration
- **ACK System**: Reliable communication with acknowledgments
- **Waterproof Sensor**: JSN-SR04T ultrasonic sensor for outdoor use

## 🏗 System Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              TRANSMITTER                                     │
│                         (On Tank - Battery Powered)                          │
│                                                                              │
│  ┌─────────────┐     ┌─────────────┐     ┌─────────────┐                    │
│  │ JSN-SR04T   │     │ ESP32-C3    │     │  RYLR998    │                    │
│  │ Ultrasonic  ├────►│ SuperMini   ├────►│   LoRa      │ )))               │
│  │ Sensor      │     │ (Deep Sleep)│     │             │                    │
│  └─────────────┘     └──────┬──────┘     └─────────────┘                    │
│                             │                                                │
│  ┌─────────────┐     ┌──────┴──────┐     ┌─────────────┐                    │
│  │  WS2812B    │     │ 3.7V 1000mAh│     │  Voltage    │                    │
│  │  2x LEDs    │     │   Battery   │     │  Divider    │                    │
│  └─────────────┘     └─────────────┘     └─────────────┘                    │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ LoRa 868/915 MHz
                                    │ (Up to 10+ km)
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                               RECEIVER                                       │
│                          (Indoor - USB Powered)                              │
│                                                                              │
│  ┌─────────────┐     ┌─────────────┐     ┌─────────────┐                    │
│  │  RYLR998    │     │ ESP32-C3    │     │  WS2812B    │                    │
│  │   LoRa      ├────►│ SuperMini   ├────►│  3x LEDs    │                    │
│  │             │     │ (WiFi ON)   │     │             │                    │
│  └─────────────┘     └──────┬──────┘     └─────────────┘                    │
│                             │                                                │
│                             ├────► MQTT → Home Assistant                     │
│                             └────► Web UI (Port 80)                          │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 🛠 Hardware Requirements

### Transmitter (Tank Side)

| Component | Quantity | Notes |
|-----------|----------|-------|
| ESP32-C3 SuperMini | 1 | Main controller |
| RYLR998 LoRa Module | 1 | 868/915 MHz |
| JSN-SR04T Ultrasonic Sensor | 1 | Waterproof |
| WS2812B LED Strip | 2 LEDs | Status indicators |
| 100KΩ Resistors | 2 | Voltage divider |
| 3.7V Li-ion Battery | 1 | 1000mAh or larger |
| TP4056 Charging Module | 1 | Optional |
| Waterproof Enclosure | 1 | IP65 or better |

### Receiver (Indoor)

| Component | Quantity | Notes |
|-----------|----------|-------|
| ESP32-C3 SuperMini | 1 | Main controller |
| RYLR998 LoRa Module | 1 | 868/915 MHz |
| WS2812B LED Strip | 3 LEDs | Status indicators |
| USB Cable | 1 | Power supply |

### Tools Needed

- Soldering iron & solder
- Wire strippers
- Multimeter
- Jumper wires

## 🔌 Wiring Diagrams

### Transmitter Wiring

```
ESP32-C3 SuperMini          RYLR998
┌──────────────────┐       ┌─────────┐
│            3.3V  ├───────┤ VDD (1) │
│             GND  ├───────┤ GND (5) │
│    GPIO21 (TX)   ├───────┤ RXD (3) │
│    GPIO20 (RX)   ├───────┤ TXD (4) │
└──────────────────┘       └─────────┘

ESP32-C3 SuperMini          JSN-SR04T
┌──────────────────┐       ┌─────────┐
│            3.3V  ├───────┤ VCC     │
│             GND  ├───────┤ GND     │
│           GPIO4  ├───────┤ TRIG    │
│           GPIO5  ├───────┤ ECHO    │
└──────────────────┘       └─────────┘

ESP32-C3 SuperMini          WS2812B (2 LEDs)
┌──────────────────┐       ┌─────────────────┐
│            3.3V  ├───────┤ VCC             │
│             GND  ├───────┤ GND             │
│           GPIO2  ├───────┤ DIN             │
└──────────────────┘       └─────────────────┘

Battery Voltage Divider (to GPIO3)
┌─────────────────────────────────────┐
│                                     │
│  Battery (+) ──┬── 100KΩ ──┬── GND  │
│                │           │        │
│                └── GPIO3 ──┘        │
│                   (ADC)             │
│                                     │
│  * Measures: 0-4.2V → 0-2.1V       │
└─────────────────────────────────────┘
```

### Receiver Wiring

```
ESP32-C3 SuperMini          RYLR998
┌──────────────────┐       ┌─────────┐
│            3.3V  ├───────┤ VDD (1) │
│             GND  ├───────┤ GND (5) │
│    GPIO21 (TX)   ├───────┤ RXD (3) │
│    GPIO20 (RX)   ├───────┤ TXD (4) │
└──────────────────┘       └─────────┘

ESP32-C3 SuperMini          WS2812B (3 LEDs)
┌──────────────────┐       ┌─────────────────────────┐
│            3.3V  ├───────┤ VCC                     │
│             GND  ├───────┤ GND                     │
│           GPIO2  ├───────┤ DIN                     │
│                  │       │  LED0: Status           │
│                  │       │  LED1: Water Level      │
│                  │       │  LED2: Signal Strength  │
└──────────────────┘       └─────────────────────────┘
```

### Tank Sensor Placement

```
     ┌──────────────────────┐
     │    JSN-SR04T Sensor  │ ← Mounted here
     └──────────┬───────────┘
               │
               │ 25cm (SENSOR_OFFSET)
               │
     ══════════╧═══════════════ ← Full tank level (100%)
               │
               │
               │ 120cm (TANK_DEPTH)
               │
               │
     ══════════════════════════ ← Empty tank level (0%)

     Minimum sensor range: 20cm
     Maximum measurable: 145cm (25 + 120)
```

## 💻 Software Setup

### Arduino IDE Setup

1. **Install Arduino IDE** (v2.0 or later recommended)

2. **Add ESP32 Board Support**
   - Go to `File` → `Preferences`
   - Add to "Additional Board Manager URLs":
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
   - Go to `Tools` → `Board` → `Boards Manager`
   - Search "esp32" and install "ESP32 by Espressif Systems"

3. **Install Required Libraries**
   
   Go to `Sketch` → `Include Library` → `Manage Libraries` and install:
   - `Adafruit NeoPixel` by Adafruit
   - `PubSubClient` by Nick O'Leary

4. **Board Settings**

   | Setting | Value |
   |---------|-------|
   | Board | ESP32C3 Dev Module |
   | USB CDC On Boot | Enabled |
   | CPU Frequency | 160MHz |
   | Flash Size | 4MB |
   | Partition Scheme | Default 4MB |

## ⚙️ Configuration

### Transmitter Configuration

Edit these values in `Transmitter/Transmitter.ino`:

```cpp
// Sleep interval (how often to send data)
#define SLEEP_MINUTES 5

// LoRa addresses
#define RECEIVER_ADDRESS 2
#define MY_ADDRESS 1
#define NETWORK_ID 6

// Frequency (use 868000000 for EU, 915000000 for US/AU)
#define FREQUENCY "915000000"

// Battery calibration
#define BATTERY_FULL_V 4.2
#define BATTERY_EMPTY_V 3.0
```

### Receiver Configuration

Edit these values in `Receiver/Receiver.ino`:

```cpp
// WiFi credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// MQTT broker settings
const char* mqtt_server = "192.168.0.163";
const int mqtt_port = 1885;
const char* mqtt_user = "mqtt-user";
const char* mqtt_password = "your-password";

// Tank dimensions (in cm)
const int TANK_DEPTH = 120;
const int SENSOR_OFFSET = 25;

// LoRa settings (must match transmitter)
#define MY_ADDRESS 2
#define NETWORK_ID 6
#define FREQUENCY "915000000"
```

## 📥 Installation

### Step 1: Flash Transmitter

1. Connect ESP32-C3 SuperMini to computer via USB
2. Open `Transmitter/Transmitter.ino` in Arduino IDE
3. Select correct board and port
4. Click Upload
5. Open Serial Monitor (115200 baud) to verify

### Step 2: Flash Receiver

1. Connect second ESP32-C3 SuperMini
2. Open `Receiver/Receiver.ino`
3. **Update WiFi and MQTT credentials**
4. Upload and verify in Serial Monitor

### Step 3: Assemble Hardware

1. Wire components according to diagrams above
2. Mount ultrasonic sensor on tank
3. Place transmitter in waterproof enclosure
4. Connect battery to transmitter
5. Power receiver via USB

### Step 4: Test Communication

1. Both devices should show activity on LEDs
2. Receiver's Status LED should turn green when data received
3. Access web dashboard at receiver's IP address
4. Verify MQTT messages in Home Assistant

## 💡 LED Status Reference

### Transmitter LEDs

| LED | Color | Meaning |
|-----|-------|---------|
| **TX (LED 0)** | 🟡 Yellow | Starting up |
| | 🔵 Blue | Sending data |
| | 🔵 Cyan | Waiting for ACK |
| | 🟢 Green (3 blinks) | Transmission successful |
| | 🔴 Red (5 blinks) | Transmission failed |
| **Battery (LED 1)** | 🟢 Green | Battery > 50% |
| | 🟡 Yellow | Battery 20-50% |
| | 🔴 Red | Battery < 20% |

### Receiver LEDs

| LED | Color | Pattern | Meaning |
|-----|-------|---------|---------|
| **Status (LED 0)** | 🟡 Yellow | Solid | Starting up |
| | 🔵 Blue | Slow pulse | Waiting for data |
| | 🟢 Green | Solid | Connected |
| | ⚪ White | Brief flash | Data received |
| | 🟡 Yellow | Blinking | Data stale (>10 min) |
| | 🔴 Red | Fast blink | Connection lost (>15 min) |
| **Water (LED 1)** | 🟢 Green | Solid | 75-100% full |
| | 🔵 Cyan | Solid | 50-74% full |
| | 🟡 Yellow | Solid | 25-49% full |
| | 🟠 Orange | Solid | 10-24% full |
| | 🔴 Red | Blinking | < 10% - Critical! |
| **Signal (LED 2)** | 🟢 Green | Solid | Excellent (> -60 dBm) |
| | 🔵 Cyan | Solid | Good (-60 to -80 dBm) |
| | 🟡 Yellow | Solid | Fair (-80 to -100 dBm) |
| | 🔴 Red | Solid | Weak (< -100 dBm) |

## 🌐 Web Dashboard

Access the web dashboard by navigating to the receiver's IP address in your browser.

### Features

- Real-time water level visualization
- Tank fill percentage with animated water
- Battery status with voltage
- Signal strength indicators
- Connection status
- Auto-refresh every 2 seconds (no manual refresh needed)

### Screenshot

The dashboard displays:
- Visual tank with water level animation
- Percentage and absolute water level
- Battery percentage and voltage
- RSSI and SNR values
- LED status indicators matching physical LEDs
- Last update timestamp

## 🏠 Home Assistant Integration

### MQTT Configuration

Add to your `configuration.yaml`:

```yaml
mqtt:
  sensor:
    - name: "Tank Water Level"
      state_topic: "tank/water_level"
      unit_of_measurement: "cm"
      icon: mdi:water
      
    - name: "Tank Water Percent"
      state_topic: "tank/water_percent"
      unit_of_measurement: "%"
      icon: mdi:water-percent
      
    - name: "Tank Raw Distance"
      state_topic: "tank/raw_distance"
      unit_of_measurement: "cm"
      icon: mdi:ruler
      
    - name: "Tank Battery Percent"
      state_topic: "tank/battery_percent"
      unit_of_measurement: "%"
      icon: mdi:battery
      device_class: battery
      
    - name: "Tank Battery Voltage"
      state_topic: "tank/battery_voltage"
      unit_of_measurement: "V"
      icon: mdi:flash
      
    - name: "Tank Signal RSSI"
      state_topic: "tank/rssi"
      unit_of_measurement: "dBm"
      icon: mdi:signal
      
    - name: "Tank Signal SNR"
      state_topic: "tank/snr"
      icon: mdi:signal-variant
      
    - name: "Tank Packets Received"
      state_topic: "tank/packets_received"
      icon: mdi:counter
      
    - name: "Tank Status"
      state_topic: "tank/status"
      icon: mdi:connection
```

### Lovelace Card Example

```yaml
type: entities
title: Water Tank
entities:
  - entity: sensor.tank_water_percent
    name: Water Level
  - entity: sensor.tank_battery_percent
    name: Battery
  - entity: sensor.tank_status
    name: Connection Status
  - entity: sensor.tank_signal_rssi
    name: Signal Strength
```

### Automation Example

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
          title: "Water Tank Alert"
          message: "Water level is low: {{ states('sensor.tank_water_percent') }}%"

  - alias: "Tank Battery Low Alert"
    trigger:
      - platform: numeric_state
        entity_id: sensor.tank_battery_percent
        below: 20
    action:
      - service: notify.mobile_app
        data:
          title: "Tank Sensor Battery"
          message: "Battery is low: {{ states('sensor.tank_battery_percent') }}%"
```

## 📊 Technical Specifications

### RYLR998 LoRa Module

| Parameter | Value |
|-----------|-------|
| Frequency | 820-960 MHz |
| TX Power | 0-22 dBm |
| Sensitivity | -129 dBm |
| Interface | UART (AT Commands) |
| Supply Voltage | 2.3-3.6V |
| TX Current | 140 mA @ 22 dBm |
| RX Current | 17.5 mA |
| Sleep Current | 10 µA |

### Communication Protocol

```
Transmitter → Receiver:
  TANK:<distance>:<battery%>:<voltage>:<msgId>
  Example: TANK:85:78:3.92:12345

Receiver → Transmitter (ACK):
  ACK:<msgId>
  Example: ACK:12345
```

### Data Flow

```
1. Transmitter wakes from deep sleep
2. Measures distance (5 readings averaged)
3. Measures battery voltage (10 readings averaged)
4. Sends data via LoRa
5. Waits for ACK (2 second timeout)
6. Retries up to 3 times if no ACK
7. Returns to deep sleep

Receiver continuously:
1. Listens for LoRa messages
2. Parses received data
3. Sends ACK back
4. Calculates water level
5. Updates LEDs
6. Publishes to MQTT
7. Serves web dashboard
```

## 🔋 Battery Life

### Power Consumption

| State | Current | Duration |
|-------|---------|----------|
| Deep Sleep | ~10 µA | ~297 seconds |
| Active (measure + transmit) | ~80 mA avg | ~3 seconds |

### Estimated Battery Life

With 1000mAh battery and 5-minute intervals:

```
Per cycle:
  Sleep: 297s × 0.01mA = 0.00083 mAh
  Active: 3s × 80mA = 0.067 mAh
  Total: ~0.068 mAh

Per hour: 12 cycles × 0.068 = 0.82 mAh
Per day: 0.82 × 24 = 19.7 mAh

Battery life: 1000 / 19.7 = ~50 days
```

### Extending Battery Life

- Increase `SLEEP_MINUTES` to 10 or 15
- Reduce TX power (`AT+CRFOP=10`)
- Use larger battery (2000-3000 mAh)
- Add solar panel with TP4056 charging

## 🔧 Troubleshooting

### No Communication

| Problem | Solution |
|---------|----------|
| No +OK from LoRa | Check TX/RX wiring (crossed?) |
| +ERR=1 | Missing \r\n - use println() |
| No messages received | Verify BAND and NETWORKID match |
| Weak signal | Check antenna, move closer |

### Sensor Issues

| Problem | Solution |
|---------|----------|
| Distance = -1 | Check sensor wiring, verify 3.3V power |
| Erratic readings | Ensure sensor is perpendicular to water |
| Always max distance | Sensor too close (< 20cm minimum) |

### WiFi/MQTT Issues

| Problem | Solution |
|---------|----------|
| WiFi won't connect | Verify SSID/password, check 2.4GHz |
| MQTT fails | Verify IP, port, credentials |
| HA not updating | Check MQTT topic names |

### LED Issues

| Problem | Solution |
|---------|----------|
| LEDs not lighting | Check DIN pin, verify 3.3V power |
| Wrong colors | WS2812B vs WS2812 - check color order |
| Dim LEDs | Increase brightness in code |

## 📁 Project Structure

```
LoRa-Water-Tank-Monitor/
├── README.md
├── LICENSE
├── Transmitter/
│   └── Transmitter.ino
├── Receiver/
│   └── Receiver.ino
├── docs/
│   ├── images/
│   │   ├── system-overview.png
│   │   ├── wiring-transmitter.png
│   │   ├── wiring-receiver.png
│   │   └── dashboard.png
│   └── RYLR998-AT-Commands.md
└── hardware/
    ├── BOM.csv
    └── enclosure-notes.md
```

## 🤝 Contributing

Contributions are welcome! Please:

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit changes (`git commit -m 'Add amazing feature'`)
4. Push to branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 🙏 Acknowledgments

- [REYAX](https://reyax.com/) for the RYLR998 LoRa module
- [Espressif](https://www.espressif.com/) for the ESP32-C3
- [Adafruit](https://www.adafruit.com/) for the NeoPixel library
- [Home Assistant](https://www.home-assistant.io/) community

## 📧 Support

If you have questions or need help:

1. Check the [Troubleshooting](#-troubleshooting) section
2. Open an [Issue](../../issues) on GitHub
3. Join the discussion in [Discussions](../../discussions)

---

**Made with ❤️ for the Home Automation community**
