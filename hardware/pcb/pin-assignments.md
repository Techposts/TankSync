# TankSync v1.0 — Pin Assignments Reference

Extracted verbatim from working firmware (`firmware/Receiver-ESP32-DevKit/main/config.h` and `firmware/Transmitter-IDF/main/config.h`). These are the **canonical** pin assignments. The schematic must match this table. If the schematic deviates, the firmware will fail at boot.

---

## TX Board — ESP32-C3 SuperMini

The TX is the outdoor solar-powered tank sensor. Carrier board accepts an **ESP32-C3 SuperMini module**; do not substitute the MCU.

| GPIO | Function | Connects to | Direction | Notes |
|---|---|---|---|---|
| **GPIO 21** | LORA_TX (UART1) | RYLR998 RXD | Output | 3.3 V logic level |
| **GPIO 20** | LORA_RX (UART1) | RYLR998 TXD | Input | 3.3 V logic level |
| **GPIO 8** | On-board status LED | LED through resistor | Output | Active-high (built into module) |
| **GPIO 7** | WS2812B data | WS2812B chain (2 LEDs in series for TX) | Output | 5V level via level shifter optional; works at 3.3 V if WS2812B is forgiving |
| **GPIO 4** | Ultrasonic TRIG | JSN-SR04T / AJ-SR04M TRIG | Output | 3.3 V — sensor is 5V powered but TRIG is fine at 3.3 V |
| **GPIO 5** | Ultrasonic ECHO | Voltage divider → JSN-SR04T ECHO | Input | **5V echo MUST be divided** (1k + 2k = 1.65 V at ECHO=5V) |
| **GPIO 9** | BOOT button | Tactile button → GND, INPUT_PULLUP | Input | 5s hold = AP mode for re-pairing |
| **GPIO 0** | Battery ADC (Variant A) | 100k + 100k divider from VBAT | Analog input | Variant A only (voltage-divider battery monitoring) |
| **GPIO 1** | I²C SDA | INA219 SDA | I/O | Variant B (INA219-based power telemetry) |
| **GPIO 2** | I²C SCL | INA219 SCL | Output | Variant B |

### TX power-path schematic (text)

```
Solar panel (+)  →  Schottky  →  CN3791 MPPT input
Solar panel (-)  →  GND
CN3791 BAT+      →  18650 holder (+)  via reverse-polarity Schottky inline
CN3791 BAT-      →  18650 holder (-)  →  GND
18650 (+)        →  Boost converter  →  3.3 V rail  →  ESP32-C3 SuperMini
                                       ↘  RYLR998 VCC
                                       ↘  Sensor 5V (via boost-to-5V or direct)
INA219 in series with battery (+) — measures both charge and discharge current.
ESP32-C3 GPIO0 reads battery voltage via 100k + 100k divider (Variant A) OR
ESP32-C3 reads INA219 over I²C at GPIO1/GPIO2 (Variant B). Auto-detects.
```

Both variants share the same firmware binary; only the BOM differs (Variant A uses 2× 100k resistors and a small cap; Variant B uses an INA219 module). Carrier should support both — populate one variant, leave the other footprints unpopulated.

---

## RX Board — ESP32 DevKit (38-pin)

The RX is the indoor USB-C-powered hub. Carrier board accepts an **ESP32 DevKit v1 (CP2102)** — 38-pin module with USB-C onboard. Module is socketed; do not redesign.

| GPIO | Function | Connects to | Direction | Notes |
|---|---|---|---|---|
| **GPIO 16** | LORA_RX (UART2) | RYLR998 TXD | Input | 3.3 V logic level |
| **GPIO 17** | LORA_TX (UART2) | RYLR998 RXD | Output | 3.3 V logic level |
| **GPIO 13** | WS2812B data | WS2812B strip (2 / 8 / 24 LEDs) | Output | **GPIO 2 is taken by onboard LED** — DO NOT use GPIO 2 for the strip |
| **GPIO 21** | I²C SDA | SH1106 OLED SDA | I/O | Standard ESP32 I²C SDA |
| **GPIO 22** | I²C SCL | SH1106 OLED SCL | Output | Standard ESP32 I²C SCL |

### RX power-path schematic (text)

```
USB-C 5V  →  bulk filter cap (10 µF + 100 nF)  →  AMS1117-3.3 LDO
                                                  ↘  3.3 V rail  →  ESP32 DevKit (3V3 pin)
                                                                  ↘  RYLR998 VCC
                                                                  ↘  SH1106 OLED VCC
USB-C 5V  →  WS2812B VCC  (with 1000 µF inrush cap close to strip)
GND       →  common ground plane
```

WS2812B runs at 5 V directly (NOT through the 3.3 V rail); this is critical for LED brightness + colour accuracy. The 1000 µF cap absorbs inrush when all LEDs go to white at full brightness simultaneously (~1 A spike).

---

## Critical PCB constraints (please verify on layout review)

- **Antenna keep-out**: >5 mm clear copper around RYLR998 SMA pad AND around the ESP32-C3 PCB antenna. We have measured RSSI degradation when copper is closer.
- **ECHO voltage divider**: TX GPIO 5 reads ECHO from a 5V sensor. Must have a 1k–2k divider on-PCB. ESP32-C3 is NOT 5V tolerant.
- **Decoupling**: every IC needs 100 nF (within 5 mm) + 10 µF (within 10 mm). Especially RYLR998 — radio modules are noise-sensitive.
- **WS2812B inrush cap**: 1000 µF electrolytic close to the LED strip's 5V input, before any in-line resistor. Reduces brown-out risk.
- **Reverse-polarity**: 18650 input on TX MUST have an inline Schottky diode. We have destroyed test boards by reversing the holder.
- **Ground plane**: keep solid pour under both digital and analog sections. Stitching vias around the antenna keep-out.

---

## What firmware will check at boot

1. I²C scan at 0x40 — if found, INA219 mode (Variant B). If not, voltage divider mode (Variant A).
2. I²C scan at 0x3C (RX only) — must succeed for OLED rendering.
3. UART2 (RX) / UART1 (TX) probe — sends `AT\r\n` to RYLR998 at 115200 baud, waits for `+OK\r\n`. If no response, falls back to setup mode.

If the schematic uses different pins for any of the above, the firmware will not boot correctly. Please align the schematic to this table; do not change pin assignments without consulting us.
