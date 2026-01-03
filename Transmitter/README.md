# LoRa Water Tank Monitor - TRANSMITTER

Battery-powered transmitter unit that measures water level using ultrasonic sensor and transmits data via LoRa. Optimized for ultra-low power consumption with deep sleep mode.

![Transmitter Hardware](../docs/Receiver%20Image.jpg)

## Hardware

- **MCU**: ESP32-C3 SuperMini
- **LoRa Module**: RYLR998 (868/915 MHz)
- **Sensor**: AJ-SR04M Waterproof Ultrasonic Sensor
- **Power**: 18650 Li-ion Battery + TP4056 Solar Charging Module
- **Indicator**: WS2812B RGB LED (1x) - GPIO powered for zero sleep current
- **Battery Monitor**: 10KΩ + 10KΩ voltage divider

## Pin Connections

### RYLR998 LoRa Module

| ESP32-C3 Pin | RYLR998 Pin | Function | Notes |
|--------------|-------------|----------|-------|
| GPIO21 | RXD (Pin 3) | UART TX | ESP TX → LoRa RX |
| GPIO20 | TXD (Pin 4) | UART RX | ESP RX ← LoRa TX |
| 3.3V | VDD (Pin 1) | Power | |
| GND | GND (Pin 5) | Ground | |

### AJ-SR04M Ultrasonic Sensor

| ESP32-C3 Pin | Sensor Pin | Function | Notes |
|--------------|------------|----------|-------|
| GPIO4 | TRIG | Trigger | 10µs pulse |
| GPIO5 | ECHO | Echo | Measures round-trip time |
| 3.3V | VCC | Power | |
| GND | GND | Ground | |

**Important**: AJ-SR04M requires minimum 100ms between measurements for stable readings.

### WS2812B LED (Status Indicator)

| ESP32-C3 Pin | LED Pin | Function | Notes |
|--------------|---------|----------|-------|
| GPIO7 | VCC | Power Control | GPIO driven for zero sleep current |
| GPIO2 | DIN | Data | |
| GND | GND | Ground | |

**Power Control**: LED power is controlled via GPIO7 to ensure complete power-off during deep sleep, achieving true zero current consumption.

### Battery Voltage Monitoring

```
Battery+ (5V) ──┬── 10KΩ ──┬── GND
                │          │
              GPIO3      10KΩ
            (ADC Input)
```

| ESP32-C3 Pin | Function | Notes |
|--------------|----------|-------|
| GPIO3 | ADC Input | Measures voltage divider mid-point |

**Voltage Range**:
- Battery range: 3.0V - 4.2V (Li-ion)
- ADC measures: 1.5V - 2.1V (divided by 2)
- Calibration factor: 0.918 (adjustable in code)

## Circuit Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        ESP32-C3 SuperMini                                │
│                                                                          │
│  ┌────────────────────────────────────────────────────────────────┐    │
│  │                                                                 │    │
│  │  [20] RX ────────────────────────────────┐                     │    │
│  │  [21] TX ───────────────────────┐        │                     │    │
│  │  3.3V ──────┬──────────┬────────┼────────┼────┐                │    │
│  │  GND ───────┼──────────┼────────┼────────┼────┼───┐            │    │
│  │             │          │        │        │    │   │            │    │
│  │  [4] TRIG ──│──────────│────────│────────│────│───│───┐        │    │
│  │  [5] ECHO ──│──────────│────────│────────│────│───│───│───┐    │    │
│  │             │          │        │        │    │   │   │   │    │    │
│  │  [7] ───────│──────────│────────│────────│────│───│───│───│─┐  │    │
│  │  [2] ───────│──────────│────────│────────│────│───│───│───│─│─┐│    │
│  │             │          │        │        │    │   │   │   │ │ ││    │
│  │  [3] ADC ───│──────────│────────│────────│────│───│───│───│─│─││─┐  │
│  │             │          │        │        │    │   │   │   │ │ ││ │  │
│  └─────────────┼──────────┼────────┼────────┼────┼───┼───┼───┼─┼─┼┼─┼──┘
│                │          │        │        │    │   │   │   │ │ ││ │
│   ┌────────────┴──┐  ┌────┴────┐ ┌┴────────┴────┴┐  │   │   │ │ ││ │
│   │  AJ-SR04M     │  │ RYLR998 │ │   WS2812B     │  │   │   │ │ ││ │
│   │               │  │  LoRa   │ │   (1 LED)     │  │   │   │ │ ││ │
│   ├───────────────┤  ├─────────┤ ├───────────────┤  │   │   │ │ ││ │
│   │ VCC     3.3V  │  │ VDD  3V │ │ VCC       GND │  │   │   │ │ ││ │
│   │ GND     GND   │  │ GND  GND│ │ GND       3.3V│  │   │   │ │ ││ │
│   │ TRIG    [4]   │  │ RXD  [21]│ │ DIN       [2]│  │   │   │ │ ││ │
│   │ ECHO    [5]   │  │ TXD  [20]│ │ PWR_CTL   [7]│  │   │   │ │ ││ │
│   └───────────────┘  └─────────┘ └───────────────┘  │   │   │ │ ││ │
│                                                      │   │   │ │ ││ │
│   ┌──────────────────────────────────────────────────┘   │   │ │ ││ │
│   │              Battery Voltage Divider                 │   │ │ ││ │
│   │                                                       │   │ │ ││ │
│   │  Battery+ (5V from TP4056) ─── 10KΩ ───┬─── 10KΩ ───┼───┘ │ ││ │
│   │                                         │            │     │ ││ │
│   │                                      GPIO3 ADC ──────┘     │ ││ │
│   │                                                             │ ││ │
│   └─────────────────────────────────────────────────────────────┘ ││ │
│                                                                    ││ │
└────────────────────────────────────────────────────────────────────┴┴─┘
                                                                     GND
```

## Power Supply

```
Solar Panel (6V)
     │
     ▼
┌─────────────────┐
│    TP4056       │  ← Solar charging module
│  (Charge IC)    │
└────────┬────────┘
         │
         ▼
  18650 Li-ion Battery
   (3.7V nominal)
         │
         ├──────────► ESP32-C3 (5V pin)
         │
         └── 10KΩ ──┬── GPIO3 (Voltage sense)
                    │
                 10KΩ
                    │
                   GND
```

## Configuration

### LoRa Settings

```cpp
#define LORA_FREQUENCY      "865000000"   // 865 MHz for India
                                          // 868 MHz for EU
                                          // 915 MHz for US/AU
#define LORA_NETWORK_ID     6             // Must match receiver (3-15 or 18)
#define LORA_TX_POWER       14            // TX power in dBm (0-22)
#define MY_ADDRESS          1             // This transmitter's address
#define RECEIVER_ADDRESS    2             // Receiver's address
```

### Sleep Configuration

```cpp
#define SLEEP_MINUTES       5             // Deep sleep duration
```

**Battery Life**: With 5-minute intervals, expect ~50 days on a 1000mAh battery.

### Battery Calibration

```cpp
#define VOLTAGE_DIVIDER_RATIO   2.0       // (R1 + R2) / R2
#define ADC_CALIBRATION_FACTOR  0.918     // Fine-tune if readings are off
#define BATTERY_FULL_VOLTAGE    4.2       // Fully charged Li-ion
#define BATTERY_EMPTY_VOLTAGE   3.0       // Safe discharge cutoff
```

### Sensor Configuration

```cpp
#define SENSOR_NUM_READINGS     7         // Take 7 readings (median used)
#define SENSOR_READING_INTERVAL 120       // 120ms between readings
#define SENSOR_TRIGGER_PULSE_US 10        // 10µs trigger pulse
#define SENSOR_ECHO_TIMEOUT_US  38000     // 38ms timeout (~6.5m max)
```

## LED Status Codes

| Color | State | Meaning |
|-------|-------|---------|
| 🟡 Yellow | Solid | Starting up |
| 🔵 Blue | Solid | Taking measurements |
| 🔵 Cyan | Solid | Transmitting data |
| 🟣 Magenta | Solid | Waiting for ACK |
| 🟢 Green | 3 blinks | Transmission successful |
| 🔴 Red | 10 blinks | LoRa initialization failed |
| 🔴 Red | 5 blinks | Transmission failed after retries |
| 🟠 Orange | Solid | Retrying transmission |

## Communication Protocol

### Data Packet Format

```
TANK:<distance>:<battery%>:<voltage>:<msgId>
```

Example:
```
TANK:85:78:3.92:1704123456
```

### ACK Format

```
ACK:<msgId>
```

### Transmission Flow

1. Wake from deep sleep
2. Initialize LoRa module
3. Take 7 ultrasonic measurements (median used)
4. Read battery voltage (10 samples averaged)
5. Send data packet to receiver
6. Wait for ACK (2 second timeout)
7. Retry up to 3 times if no ACK
8. Enter deep sleep for configured duration

## Power Consumption

| State | Current | Duration (5min cycle) |
|-------|---------|----------------------|
| Deep Sleep | ~10 µA | ~297 seconds |
| Active (measure + TX) | ~80 mA avg | ~3 seconds |

**Estimated Battery Life**:
- 1000mAh battery: ~50 days
- 2000mAh battery: ~100 days
- With solar panel: Indefinite (if solar input > daily consumption)

## Installation

### 1. Flash Firmware

```bash
# Install Arduino IDE and ESP32 board support
# Install libraries:
# - Adafruit NeoPixel

# Board Settings:
# - Board: ESP32C3 Dev Module
# - USB CDC On Boot: Enabled
# - Flash Size: 4MB
# - Partition Scheme: Default 4MB

# Upload sketch
```

### 2. Configure Settings

Edit the configuration section in `Transmitter.ino`:
- Set LoRa frequency for your region
- Match network ID and addresses with receiver
- Adjust sleep interval as needed
- Calibrate voltage divider if necessary

### 3. Hardware Assembly

1. Wire all components according to pin diagram
2. Mount ultrasonic sensor on tank (sensor facing water)
3. Connect battery through TP4056 module
4. Place in waterproof enclosure (IP65 or better)
5. Optional: Connect solar panel to TP4056

### 4. Testing

1. Open Serial Monitor (115200 baud)
2. Verify LoRa initialization (should show "+OK" responses)
3. Check sensor readings (should be stable)
4. Verify battery voltage reading
5. Confirm successful transmission (green LED blinks)
6. Measure actual sleep current (~10µA)

## Troubleshooting

### LoRa Not Responding

- Check TX/RX wiring (pins may be crossed)
- Verify 3.3V power supply
- Ensure antenna is connected
- Check Serial1 baud rate (115200)

### Sensor Readings Invalid

- Verify sensor has 3.3V power
- Check TRIG and ECHO pin connections
- Ensure sensor is perpendicular to water surface
- Minimum distance: 20cm
- Maximum distance: 600cm

### Battery Voltage Incorrect

- Check voltage divider resistor values (both 10KΩ)
- Adjust `ADC_CALIBRATION_FACTOR` in code
- Measure actual battery voltage with multimeter
- Verify ADC is reading ~half of battery voltage

### LED Not Working

- Verify GPIO7 is controlling power
- Check GPIO2 connection to DIN
- Ensure WS2812B is powered from GPIO7
- Check for correct NeoPixel library configuration

### High Sleep Current

- Ensure LED power is off (GPIO7 LOW)
- Verify WiFi and Bluetooth are disabled
- Check for floating pins
- Measure with multimeter in series with battery

## Calibration Procedure

### 1. Battery Voltage Calibration

```bash
# 1. Fully charge battery
# 2. Measure actual voltage with multimeter (e.g., 4.18V)
# 3. Check reading in serial monitor
# 4. Calculate adjustment factor:
#    FACTOR = actual_voltage / displayed_voltage
# 5. Update ADC_CALIBRATION_FACTOR in code
```

### 2. Distance Calibration

```bash
# 1. Place sensor at known distance (e.g., 50cm from flat surface)
# 2. Compare sensor reading with actual distance
# 3. Adjust DISTANCE_OFFSET_CM if consistent error
```

## Extending Battery Life

1. **Increase sleep interval**: Change `SLEEP_MINUTES` from 5 to 10 or 15
2. **Reduce TX power**: Lower `LORA_TX_POWER` from 14 to 10 dBm (if signal allows)
3. **Reduce sensor readings**: Change `SENSOR_NUM_READINGS` from 7 to 5
4. **Add solar charging**: 5V 100mA solar panel maintains charge
5. **Use larger battery**: 2000-3000mAh batteries available

## Technical Specifications

### AJ-SR04M Sensor
- Working voltage: 3.3V - 5V
- Working current: 30mA
- Range: 20cm - 600cm
- Accuracy: ±1cm
- Angle: 75°
- Waterproof rating: IP67

### RYLR998 LoRa Module
- Frequency: 820-960 MHz (region dependent)
- Sensitivity: -148 dBm
- Output power: 0-22 dBm (configurable)
- Interface: UART (AT commands)
- Range: Up to 10km (line of sight)

### ESP32-C3 SuperMini
- CPU: RISC-V 32-bit @ 160MHz
- RAM: 400KB SRAM
- Flash: 4MB
- Deep sleep current: ~10µA
- ADC resolution: 12-bit
- Operating voltage: 3.3V

## License

MIT License - See main repository LICENSE file

## Support

For issues and questions, please open an issue on the main repository.
