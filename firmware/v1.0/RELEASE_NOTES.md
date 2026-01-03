# Release v1.0 - LoRa Water Tank Monitor

**Release Date**: January 2025
**Board**: ESP32-C3 SuperMini
**LoRa Module**: RYLR998 (868/915 MHz)

---

## 📦 Download

### Pre-compiled Binaries

| Component | File | Size | MD5 Checksum |
|-----------|------|------|--------------|
| **Transmitter** | `Transmitter_ESP32C3_v1.0.bin` | 4.0 MB | - |
| **Receiver** | `Receiver_ESP32C3_v1.0.bin` | 4.0 MB | - |

### Flashing Instructions

See [FLASHING.md](../../FLASHING.md) for detailed instructions.

**Quick Flash** (using esptool):
```bash
# Transmitter
esptool.py --chip esp32c3 --port COM3 --baud 921600 write_flash 0x0 Transmitter_ESP32C3_v1.0.bin

# Receiver
esptool.py --chip esp32c3 --port COM3 --baud 921600 write_flash 0x0 Receiver_ESP32C3_v1.0.bin
```

---

## ✨ Features

### Transmitter

- **Ultra Low Power**: Deep sleep mode with ~10µA current draw
- **Battery Life**: 50+ days on 1000mAh battery (5-minute intervals)
- **Solar Compatible**: Works with TP4056 solar charging module
- **Waterproof Sensor**: AJ-SR04M ultrasonic sensor (IP67 rated)
- **LED Status Indicator**: WS2812B RGB LED with GPIO power control
- **Battery Monitoring**: Accurate voltage and percentage via ADC
- **Reliable Communication**: ACK-based transmission with retry logic
- **Long Range**: Up to 10+ km line-of-sight via LoRa

#### Pin Configuration
| Pin | Function | Component |
|-----|----------|-----------|
| GPIO21 | UART TX | RYLR998 RXD |
| GPIO20 | UART RX | RYLR998 TXD |
| GPIO4 | Trigger | AJ-SR04M TRIG |
| GPIO5 | Echo | AJ-SR04M ECHO |
| GPIO3 | ADC | Battery voltage (10KΩ divider) |
| GPIO7 | Power | WS2812B LED power control |
| GPIO2 | Data | WS2812B LED data |

#### Default Settings
```
LORA_FREQUENCY:      865000000 (865 MHz - India)
LORA_NETWORK_ID:     6
MY_ADDRESS:          1
RECEIVER_ADDRESS:    2
SLEEP_MINUTES:       5
TX_POWER:            14 dBm
```

**Note**: To change settings, you must edit code and recompile.

---

### Receiver (Modular Architecture)

- **Modular Code Structure**: Clean separation of concerns
- **OLED Display**: 128x64 SSD1306 with 4 rotating screens
- **Dual LED Status**: Separate indicators for water level and system status
- **Web Dashboard**: Real-time monitoring with auto-refresh
- **WiFi Management**: Web-based configuration with AP fallback mode
- **MQTT Publishing**: Home Assistant integration
- **Data Persistence**: Settings saved in flash (NVS)
- **Watchdog Timer**: Proper WDT handling to prevent crashes

#### Pin Configuration
| Pin | Function | Component |
|-----|----------|-----------|
| GPIO21 | UART TX | RYLR998 RXD |
| GPIO20 | UART RX | RYLR998 TXD |
| GPIO9 | I2C SDA | SSD1306 OLED |
| GPIO10 | I2C SCL | SSD1306 OLED |
| GPIO2 | Data | WS2812B LEDs (2 LEDs) |

#### Default Settings
```
LORA_FREQUENCY:      865000000 (865 MHz)
LORA_NETWORK_ID:     6
MY_ADDRESS:          2
MQTT_SERVER:         192.168.0.163
MQTT_PORT:           1885
AP_SSID:             TankSync
```

**Configuration**: Can be changed via web interface at `http://192.168.4.1/config`

---

## 🎨 LED Status Indicators

### Transmitter (1 LED)

| Color | Meaning |
|-------|---------|
| 🟡 Yellow | Starting up |
| 🔵 Blue | Taking measurements |
| 🔵 Cyan | Transmitting |
| 🟣 Magenta | Waiting for ACK |
| 🟢 Green (3 blinks) | Success! |
| 🔴 Red (5 blinks) | Failed |

### Receiver (2 LEDs)

**LED 0 - Water Level**:
- 🟢 Green: 75-100% (Full)
- 🔵 Cyan: 50-74% (Good)
- 🟡 Yellow: 25-49% (Fair)
- 🟠 Orange: 10-24% (Low)
- 🔴 Red (blinking): 0-9% (Critical)

**LED 1 - System Status**:
- 🟢 Green: Connected
- 🟡 Yellow: Data stale (>10 min)
- 🔴 Red: LoRa error
- 🔵 Blue (blinking): AP mode

---

## 🔧 What's Included

### Transmitter Binary Contains:
- ✅ ESP32-C3 bootloader
- ✅ Partition table
- ✅ Complete application firmware
- ✅ Adafruit NeoPixel library
- ✅ Deep sleep power management
- ✅ LoRa communication stack
- ✅ Battery monitoring
- ✅ Ultrasonic sensor driver

### Receiver Binary Contains:
- ✅ ESP32-C3 bootloader
- ✅ Partition table
- ✅ Complete application firmware
- ✅ WiFi manager with AP mode
- ✅ Web server with dashboard
- ✅ MQTT client (PubSubClient)
- ✅ OLED display driver (Adafruit SSD1306)
- ✅ NeoPixel library
- ✅ LoRa communication stack
- ✅ NVS storage for settings

---

## 📊 OLED Display Screens

The receiver cycles through 4 screens every 8 seconds:

### Screen 1: Water Level
```
┌──────────────┐
│ Water: 85%   │
│ ┌──────────┐ │
│ │▓▓▓▓▓▓    │ │ ← Animated tank
│ │▓▓▓▓▓▓    │ │
│ └──────────┘ │
│  425 Liters  │
└──────────────┘
```

### Screen 2: Battery
```
┌──────────────┐
│ Battery      │
│   78%        │
│   3.92V      │
│ [████████░░] │
└──────────────┘
```

### Screen 3: Signal
```
┌──────────────┐
│ Signal       │
│ RSSI: -65dBm │
│ SNR:  9 dB   │
│ ████████░░   │
└──────────────┘
```

### Screen 4: Stats
```
┌──────────────┐
│ Uptime:      │
│  2d 5h 32m   │
│ Packets: 578 │
│ Success: 99% │
└──────────────┘
```

---

## 🌐 Web Dashboard

Access via `http://<receiver-ip>/`

**Features**:
- Real-time water level with animated tank graphic
- Battery voltage and percentage
- Signal strength (RSSI, SNR)
- Connection status
- Last update timestamp
- Auto-refresh every 2 seconds

**Configuration Page**: `http://<receiver-ip>/config`
- WiFi credentials
- MQTT broker settings
- Tank calibration
- LoRa settings
- Alert thresholds

---

## 🏠 Home Assistant Integration

### MQTT Topics Published

```
tank/water_level        → Water level in cm
tank/water_percent      → Water percentage (0-100)
tank/raw_distance       → Raw sensor reading
tank/battery_percent    → Transmitter battery %
tank/battery_voltage    → Transmitter battery voltage
tank/rssi              → Signal strength (dBm)
tank/snr               → Signal-to-noise ratio
tank/status            → Connection status
tank/packets_received   → Total packets
```

Publishing occurs **only when new data arrives** (not on fixed interval).

---

## 🔋 Power Consumption

### Transmitter (5-minute cycle)

| State | Current | Duration |
|-------|---------|----------|
| Deep Sleep | 10 µA | 297 sec |
| Active | 80 mA avg | 3 sec |

**Battery Life**:
- 1000mAh: ~51 days
- 2000mAh: ~102 days
- 3000mAh: ~153 days

### Receiver (USB Powered)

Continuous operation, no sleep mode.

---

## ⚠️ Known Issues

### Transmitter
- Settings are hardcoded - requires recompilation to change
- No OTA update support yet
- LED brightness not adjustable without recompile

### Receiver
- LoRa settings must be changed via web interface and match transmitter
- OLED contrast not adjustable
- No user authentication on web interface
- MQTT reconnect can take up to 30 seconds

---

## 🆕 What's New in v1.0

This is the initial stable release with:

### Transmitter
- ✅ Deep sleep optimization
- ✅ GPIO-controlled LED power for zero sleep current
- ✅ Median-based sensor readings for stability
- ✅ Voltage divider battery monitoring with calibration
- ✅ ACK-based reliable transmission
- ✅ Automatic retry logic (up to 3 attempts)

### Receiver
- ✅ Modular code architecture
- ✅ Watchdog timer fix (no more random reboots)
- ✅ Reduced LED count from 3 to 2 for simplicity
- ✅ WiFi configuration via web interface
- ✅ MQTT publishing on data arrival (not timed interval)
- ✅ OLED display with 4 rotating screens
- ✅ Web dashboard with real-time updates
- ✅ NVS storage for persistent settings

---

## 🔮 Future Roadmap

Planned for v2.0:
- [ ] OTA firmware updates
- [ ] Web-based transmitter configuration
- [ ] User authentication for web interface
- [ ] Email/Telegram alerts
- [ ] Multiple tank support (multi-transmitter)
- [ ] Historical data logging
- [ ] Adjustable LED brightness
- [ ] BLE configuration for transmitter
- [ ] Battery charge rate monitoring

---

## 🐛 Bug Fixes Since Last Version

N/A - This is the initial release

---

## 📜 Changelog

### v1.0 (January 2025)
- Initial stable release
- Transmitter with deep sleep and battery monitoring
- Receiver with modular architecture
- Web dashboard and MQTT integration
- Comprehensive documentation

---

## 🛠 Compilation Info

**Compiled with**:
- Arduino IDE 2.x
- ESP32 Board Package: 2.0.x
- Adafruit NeoPixel: Latest
- Adafruit GFX: Latest
- Adafruit SSD1306: Latest
- PubSubClient: Latest

**Board Settings**:
```
Board: ESP32C3 Dev Module
USB CDC On Boot: Enabled
CPU Frequency: 160MHz
Flash Size: 4MB
Partition Scheme: Default 4MB with spiffs
Upload Speed: 921600
```

---

## 📄 License

MIT License - Free to use, modify, and distribute with attribution.

---

## 🙏 Support

- **Documentation**: [README.md](../../README.md)
- **Flashing Guide**: [FLASHING.md](../../FLASHING.md)
- **Issues**: [GitHub Issues](https://github.com/Techposts/LoRa-Water-Tank-Monitor/issues)
- **Discussions**: [GitHub Discussions](https://github.com/Techposts/LoRa-Water-Tank-Monitor/discussions)

---

**Enjoy your LoRa Water Tank Monitor! 💧**
