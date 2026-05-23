# TankSync Hardware Wiring & Pin Connections

Authoritative wiring reference for the TankSync receiver (RX) and transmitter (TX) boards. Pin assignments here match `firmware/<target>/main/config.h` exactly; if the two ever disagree, the `config.h` file is the source of truth and this doc must be updated.

## Topology overview

| Device | Power source | Where it lives | Duty cycle |
|---|---|---|---|
| **RX** (receiver / display unit) | 5V USB-C wall brick (≥1.5A) → on-board 3.3V LDO | Indoors, near a power outlet, within WiFi range | Always-on; WiFi STA + LoRa RX listening continuously |
| **TX** (transmitter / sensor unit) | 6V solar panel → CN3791 MPPT charger → 18650 Li-ion → MT3608 boost → 5V rail | Outdoors, mounted on or near the water tank, often far from the house | Deep-sleep cycle: wakes every 5 min, samples ultrasonic + LoRa burst (~3s awake), sleeps |

The TX is the energy-constrained device. The RX is intentionally indoor-and-plugged so its power budget (display + WiFi + LoRa-listen + LEDs) doesn't have to fit a small solar harvest.

### Whole-system flow (5 boxes)

```mermaid
flowchart LR
    TX[TX board<br/>by tank] -.->|LoRa 865 MHz| RX[RX board<br/>indoors]
    RX -->|WiFi| BROKER[MQTT broker<br/>HA or TankSync cloud]
    BROKER --> APP[Dashboard<br/>HA or PWA]
```

All modules share a common ground. Detailed power chain, sensor connections, and per-board pin maps are below.

---

## Sensor naming note (TX)

The firmware refers to the ultrasonic distance sensor as **AJ-SR04M**. Same module is also sold as **JSN-SR04M-2**, **HC-SR04M-WP**, and **RCWL-9620** — functionally identical (5V TRIG/ECHO protocol, waterproof transducer on a cable). Any of these will work; the cloud BOM uses AJ-SR04M part numbers because that's what the Robu/Indian distributors carry. **ECHO is a 5V output** and must be level-shifted to 3.3V before reaching the C3's GPIO5 (1kΩ series + 2kΩ to GND on PCB; on breadboard you can get away without it short-term but a production PCB MUST include the divider).

---

## User-interaction model (both boards)

For PCB design, both boards expose two physical user buttons + one status LED. The exact firmware behaviours below are what the firmware roadmap targets; the **physical wiring** is what the PCB needs to support today regardless of when each firmware behaviour ships.

| Board | Button A (BOOT/long-press) | Button B (RESET) | Status LED |
|---|---|---|---|
| **TX** | GPIO9 onboard BOOT (hold 2s = pair, hold 5s = AP mode for OTA) | EN pin (hardware reset) | GPIO8 onboard single LED (visual feedback during pair / AP / OTA states) |
| **RX** | GPIO0 onboard BOOT (hold 2s = pair, hold 5s = AP mode for OTA) | EN pin (hardware reset) | WS2812 chain on GPIO13 (firmware-configurable count: 2 / 8 / 24) |

**Important strapping-pin caveats** (both boards):
- GPIO0 (RX BOOT) and GPIO9 (TX BOOT) are *strapping pins* — they must read HIGH at reset for the chip to enter normal flash boot. The momentary tactile button (only pulls LOW when pressed) is fine; the dev-board's onboard 10kΩ pull-up + your external panel button in parallel both default to HIGH. Do NOT add anything that holds these pins LOW persistently.
- TX GPIO8 (onboard LED) is also a strapping pin (must be HIGH at boot for normal flash mode). The C3 SuperMini's onboard LED has a series resistor and the line is high-Z when not driven, so boot is normally fine. If you ever see the chip refusing to boot on a custom PCB layout, add an external 10kΩ pull-up from GPIO8 to 3V3.

**Firmware-state-to-LED-colour mapping (TX status LED on GPIO8 — planned)**:

| Firmware state | LED behaviour |
|---|---|
| Normal sleep/wake cycle | Off (deep-sleep saves power) |
| Pairing mode (after 2s hold) | Slow blink, ~1Hz |
| AP mode for OTA (after 5s hold) | Fast blink, ~3Hz |
| OTA flash in progress | Solid on |
| Pairing successful | 3 quick flashes, then off |

The same state machine on RX uses the WS2812 chain instead of a single LED — colours map to states with the broader palette (rain blue = pairing, leaf green = paired, warm = AP, rust = error).

---

## Receiver (RX) wiring

There are **two RX hardware variants** depending on the ESP32 chip on hand:
- **DevKit ESP32 v1** (CP2102, 30-pin header) — full-size, more GPIOs, easier to breadboard
- **ESP32-C3 SuperMini** — pocket-sized, fewer GPIOs

Both variants use the **same external modules** (RYLR998 LoRa, SH1106 OLED, WS2812B LEDs). Only the GPIO assignments differ. Power comes from a 5V USB-C wall brick into the dev-board's USB connector; the on-board AMS1117-3.3 LDO derives the 3.3V rail used by RYLR998.

### RX power flow (4 boxes)

```mermaid
flowchart LR
    USB[5V USB-C brick<br/>≥2A for 24-ring] --> ESP[ESP32 DevKit<br/>onboard AMS1117 → 3V3]
    USB -->|+5V| LEDS[WS2812B chain<br/>2/8/24 LEDs]
    ESP -->|3V3| OLED[SH1106 OLED]
    ESP -->|3V3| LORA[RYLR998 LoRa]
```

WS2812 chain takes its **VCC from USB 5V directly**, NOT from the DevKit's 3V3 LDO. A 24-LED ring at full white draws ~1.4A — far beyond the AMS1117's ~600mA budget. Pulling LED current from the 5V rail keeps the MCU power clean. Add a **1000µF electrolytic capacitor across the LED chain's VCC↔GND at the input** to absorb inrush when the strip lights up — important for the 24-ring, recommended for the 8-strip.

### RX peripheral connections (5 boxes — split for clarity)

```mermaid
flowchart LR
    ESP[ESP32 DevKit] <-->|UART2<br/>GPIO16/17| LORA[RYLR998 LoRa]
    ESP <-->|I²C 0x3C<br/>GPIO21/22| OLED[SH1106 OLED]
    ESP -->|GPIO13<br/>+ 470Ω in line| LEDS[WS2812B chain]
```

```mermaid
flowchart LR
    BOOT[BOOT button<br/>onboard + external] -->|GPIO0 active LOW| ESP[ESP32 DevKit]
    RST[RESET button<br/>onboard + external] -->|EN pin| ESP
```

All modules share a common ground.

### RX block diagram (alt-text view)

```
 [5V USB-C wall brick, ≥1.5A]
            │
            ▼
   ┌──────────────────────┐
   │  ESP32 (DevKit / C3) │
   │  USB connector       │
   └─┬──────────────────┬─┘
     │ 5V rail          │ 3.3V rail (from on-board LDO)
     ▼                  ▼
 ┌────────────┐    ┌────────────┐    ┌──────────────────┐
 │ WS2812B ×2 │    │ RYLR998    │    │ SH1106 OLED      │
 │ (status +  │    │ LoRa       │    │ 128×64, I²C 0x3C │
 │  water lvl)│    │ UART @     │    │ 3.3V or 5V       │
 │ VDD = 5V   │    │ 115200     │    │ tolerant module  │
 │ DIN = GPIO │    │ VCC = 3.3V │    │ SDA, SCL pins    │
 └────────────┘    └────────────┘    └──────────────────┘
```

### Pin map — DevKit ESP32 RX

Source of truth: `cloud/firmware/Receiver-ESP32-DevKit/main/config.h` (the public-tree `firmware/receiver/main/config.h` mirrors this for the open-core build). If they ever disagree, **cloud config.h wins**.

| GPIO | Function | Connects to | Notes |
|---|---|---|---|
| 0 | BOOT button (planned) | Onboard tactile + external panel button → GND | Strapping pin; momentary press only. Firmware reads after boot for hold-2s/hold-5s pattern (planned). |
| 2 | Active buzzer (optional, RX 2.8.0+) | Buzzer SIGNAL pin | **Strapping pin** — disconnect during USB flashing or chip stays in download mode. OTA flashing unaffected. Shares onboard blue LED — LED pulses with buzzer. |
| 13 | WS2812B data | DIN of first LED in chain (2/8/24-config) | 3.3V data line; add 470Ω in series to dampen reflections. 74AHCT125 level shifter only if flicker observed. |
| 16 | UART2 RX | RYLR998 TXD | DevKit has UART2 free; C3 doesn't |
| 17 | UART2 TX | RYLR998 RXD | |
| 21 | I²C SDA | SH1106 SDA | Standard I²C bus |
| 22 | I²C SCL | SH1106 SCL | |
| EN | RESET button | Onboard tactile + external panel button → GND | Hardware reset; pulled HIGH by board |
| 5V | Power | WS2812B VDD (direct from USB, NOT through 3V3 LDO) | 24-ring needs 2A wall brick |
| 3.3V | Power | RYLR998 VCC, SH1106 OLED VCC, buzzer VCC | From on-board LDO |
| GND | Ground | All modules | Single common ground |

#### Wire-by-wire — DevKit ESP32 RX

Every wire you actually solder/breadboard, in order:

```
# POWER
USB-C brick 5V         →  ESP32 DevKit VIN (or 5V pin)
USB-C brick 5V         →  WS2812B chain VDD  (direct, NOT through DevKit 3V3)
ESP32 DevKit 3V3       →  RYLR998 VCC
ESP32 DevKit 3V3       →  SH1106 OLED VCC  (most modules accept 3V3 or 5V)
1000µF cap             →  across WS2812 chain VCC↔GND at input  (inrush absorber)

# WS2812 LED CHAIN (firmware config: 2 / 8 / 24 LEDs)
ESP32 DevKit GPIO13    →  470Ω resistor → WS2812 #1 DIN
WS2812 #1 DOUT         →  WS2812 #2 DIN
WS2812 #2 DOUT         →  WS2812 #3 DIN  ... (chain continues for 8-strip or 24-ring)

# LORA UART (UART2)
ESP32 DevKit GPIO16 (RX2) →  RYLR998 TXD
ESP32 DevKit GPIO17 (TX2) →  RYLR998 RXD
                             RYLR998 RST  (leave floating, or pull-up to 3V3 via 10kΩ)

# I²C BUS (OLED display)
ESP32 DevKit GPIO21    →  SH1106 OLED SDA
ESP32 DevKit GPIO22    →  SH1106 OLED SCL
                          (most OLED modules have onboard 4.7kΩ pull-ups; if not, add to 3V3)

# BUZZER (optional — RX 2.8.0+ enables audible alerts)
# Active 3-pin buzzer module (has on-board oscillator — no PWM needed).
# Plays boot tone + low-water + overflow + sensor-offline alerts.
ESP32 DevKit GPIO2     →  Buzzer SIGNAL (I/O) pin
ESP32 DevKit 3V3       →  Buzzer VCC
ESP32 DevKit GND       →  Buzzer GND
# !! STRAPPING PIN CAVEAT: GPIO2 must read HIGH at reset for normal
# !! boot. If the buzzer holds the line LOW (or pulls the rail down
# !! during USB connect), the chip enters ROM download mode and won't
# !! run firmware. For USB flashing: DISCONNECT the signal wire from
# !! GPIO2, flash, then reconnect. OTA (over-WiFi) reflashing is
# !! unaffected — the chip re-reads strapping only at hard reset, not
# !! during the OTA reboot path.

# USER INTERACTION (PCB-ready; firmware support pending)
External panel button  →  ESP32 DevKit GPIO0 (parallel with onboard BOOT button) → GND
External panel button  →  ESP32 DevKit EN pin (parallel with onboard RST button) → GND

# COMMON GROUND — all module GNDs land on one net
```

### Pin map — ESP32-C3 SuperMini RX

Source: `firmware/receiver-c3/main/config.h`

| GPIO | Function | Connects to | Notes |
|---|---|---|---|
| 2 | WS2812B data | DIN of first LED in chain | |
| 9 | I²C SDA | SH1106 SDA | C3 GPIO matrix routes I²C anywhere |
| 10 | I²C SCL | SH1106 SCL | |
| 20 | UART1 RX | RYLR998 TXD | C3 only has UART0 (USB) and UART1 |
| 21 | UART1 TX | RYLR998 RXD | |
| 5V | Power | WS2812B VDD, OLED VCC (5V variant) | From USB |
| 3.3V | Power | RYLR998 VCC, OLED VCC (3.3V variant) | From on-board LDO |
| GND | Ground | All modules | Single common ground |

#### Wire-by-wire — ESP32-C3 SuperMini RX

```
C3 SuperMini GPIO2        →  WS2812B #1 DIN
C3 SuperMini 5V           →  WS2812B #1 VDD
C3 SuperMini GND          →  WS2812B #1 GND
WS2812B #1 DOUT           →  WS2812B #2 DIN  (chain)
WS2812B #1 VDD            →  WS2812B #2 VDD
WS2812B #1 GND            →  WS2812B #2 GND

C3 SuperMini GPIO20 (RX1) →  RYLR998 TXD
C3 SuperMini GPIO21 (TX1) →  RYLR998 RXD
C3 SuperMini 3.3V         →  RYLR998 VCC
C3 SuperMini GND          →  RYLR998 GND

C3 SuperMini GPIO9        →  SH1106 OLED SDA
C3 SuperMini GPIO10       →  SH1106 OLED SCL
C3 SuperMini 3.3V or 5V   →  SH1106 OLED VCC
C3 SuperMini GND          →  SH1106 OLED GND
```

---

## Transmitter (TX) wiring — solar-powered, two power-monitoring variants

The TX is `firmware/transmitter/` (ESP32-C3 SuperMini). The board ships in two variants depending on the level of power-monitoring telemetry desired:

- **Variant A — Voltage divider only** (basic): battery percentage from ADC reading. Cheapest BOM. Used in the entry-level product SKU.
- **Variant B — INA219 over I²C** (full): bidirectional current, accurate bus voltage, and computed power. Used in the premium SKU and required for any cloud-side energy analytics.

The firmware **auto-detects** which variant is installed by I²C-scanning at INA219's default address (`0x40`) at boot. Users can also force a mode via the web UI dropdown (Auto / Force INA219 / Force voltage divider / Disabled). The mode is saved to NVS.

### TX power chain — common to both variants (5 boxes)

```mermaid
flowchart LR
    PANEL[Solar panel<br/>6V / 180mA] --> MPPT[CN3791<br/>MPPT charger]
    MPPT --> BAT[18650 Li-ion<br/>protected]
    BAT --> BOOST[MT3608 boost<br/>3.7V → 5V]
    BOOST --> RAIL[+5V rail to<br/>ESP32-C3, sonar, LEDs]
```

### TX peripheral connections (5 boxes)

```mermaid
flowchart LR
    ESP[ESP32-C3 SuperMini] <-->|UART| LORA[RYLR998 LoRa]
    ESP <-->|TRIG/ECHO| SONAR[AJ-SR04M sonar]
    ESP -->|GPIO data| LEDS[WS2812B × 2]
    ESP <-->|GPIO9| BTN[Tactile button]
```

### TX power-monitor variants (where Variant A and B differ)

```mermaid
flowchart LR
    BAT[18650 battery+] -- Variant A --> DIV[100k/100k divider<br/>→ GPIO0 ADC]
    BAT -- Variant B --> INA[INA219 shunt<br/>I²C 0x40 SDA/SCL = GPIO1/2]
    DIV --> COMMON[battery+ continues to<br/>CN3791 BAT and MT3608 in]
    INA --> COMMON
```

All modules share a common ground. Variant A and Variant B run the **same firmware binary** — the firmware boot-probes I²C `0x40` to pick the active mode.

### TX power chain (alt-text view)

```
   ┌──────────────────────┐
   │  Solar panel         │   6V open-circuit, 180mA short-circuit
   │  (6V / 180mA / ~1W)  │   (Robu / generic crystalline)
   └──────────┬───────────┘
              │ Vin
              ▼
   ┌──────────────────────┐
   │  CN3791              │   MPPT, tracks Vmpp ~4.5V on this panel
   │  MPPT Solar Charger  │   ~85% efficiency under MPPT
   │  (Robu listing)      │   R_PROG sets charge current (default ~500mA)
   └──────────┬───────────┘
              │ BAT± to cell
              ▼
   ┌──────────────────────┐
   │  18650 Li-ion        │   3000 mAh, with PCB protection circuit
   │  (3.0–4.2V)          │   (mandatory — CN3791 has no over-discharge protection)
   └──────────┬───────────┘
              │
              ├─► [Variant A: voltage divider 100k/100k → GPIO0 (ADC1_CH0)]
              │
              ├─► [Variant B: INA219 in series with battery+ → I²C to ESP32]
              │
              ▼
   ┌──────────────────────┐
   │  MT3608 Boost        │   3.7V → 5V regulated output
   │  3.7V → 5V           │   ~85% efficiency
   │  EN tied high (or to GPIO10 for low-power deep-sleep gating)
   └──────────┬───────────┘
              │ +5V rail
              ▼
   ┌──────────┴────────────────────────────────────────────┐
   ▼                  ▼              ▼              ▼
 ┌────────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐
 │ ESP32-C3   │  │ WS2812B  │  │ AJ-SR04M │  │ Common   │
 │ SuperMini  │  │ ×2       │  │ Sonar    │  │ GND      │
 │ VBUS = 5V  │  │ VDD = 5V │  │ VCC = 5V │  │          │
 └─────┬──────┘  │ DIN = G7 │  │ TRIG = G4│  └──────────┘
       │        └──────────┘  │ ECHO = G5│
       │                      └──────────┘
       │ on-board AMS1117-3.3 LDO
       ▼
   3.3V rail → RYLR998 LoRa (VCC, RXD = GPIO21, TXD = GPIO20)
```

### TX pin map — Variant A (voltage divider only)

Source: `firmware/transmitter/main/config.h` (BAT_ADC_CHANNEL = 0)

| GPIO | Function | Connects to |
|---|---|---|
| 0 | ADC1_CH0 — battery voltage | Junction of 100kΩ / 100kΩ divider on Vbat (battery+ → 100k → GPIO0 → 100k → GND) |
| 4 | Ultrasonic TRIG | AJ-SR04M TRIG |
| 5 | Ultrasonic ECHO | AJ-SR04M ECHO |
| 7 | WS2812B data | DIN of first WS2812B (2 LEDs in series) |
| 8 | On-board LED | Built-in (no external) |
| 9 | Button | Tactile switch to GND, internal pull-up |
| 20 | UART1 RX | RYLR998 TXD |
| 21 | UART1 TX | RYLR998 RXD |
| 1, 2, 3, 6, 10 | **FREE** | Reserved for future expansion |

#### Wire-by-wire — TX Variant A

Power chain wires:

```
Solar panel (+)            →  CN3791 IN+
Solar panel (–)            →  CN3791 IN–
CN3791 BAT+                →  18650 cell positive (via protection PCB)
CN3791 BAT–                →  18650 cell negative (via protection PCB)

18650 BAT+ (post-protect)  →  Junction A — splits to 3 places:
                              (1) 100kΩ resistor → GPIO0 → 100kΩ → GND  (divider)
                              (2) MT3608 boost VIN+
18650 BAT– (post-protect)  →  Common GND  (also MT3608 GND, panel GND)

MT3608 VOUT+               →  +5V rail — splits to:
                              (1) C3 SuperMini 5V (VBUS)
                              (2) WS2812B #1 VDD
                              (3) AJ-SR04M VCC
MT3608 VOUT–               →  Common GND
MT3608 EN (optional)       →  C3 SuperMini GPIO10 (recommended for sleep gating)
```

Signal/sensor wires:

```
C3 SuperMini GPIO0         →  Voltage-divider midpoint (described above)
C3 SuperMini GPIO4         →  AJ-SR04M TRIG
C3 SuperMini GPIO5         →  AJ-SR04M ECHO
C3 SuperMini GPIO7         →  WS2812B #1 DIN
WS2812B #1 DOUT            →  WS2812B #2 DIN  (chain)
C3 SuperMini GPIO9         →  Tactile button → GND  (uses internal pull-up)
C3 SuperMini GPIO20 (RX1)  →  RYLR998 TXD
C3 SuperMini GPIO21 (TX1)  →  RYLR998 RXD
C3 SuperMini 3.3V (output) →  RYLR998 VCC
C3 SuperMini GND           →  Common GND  (and all module GNDs)
```

### TX pin map — Variant B (INA219 over I²C)

Adds I²C bus on GPIO1/2; voltage divider is **not installed** (INA219's bus-voltage register replaces it).

| GPIO | Function | Connects to |
|---|---|---|
| 0 | (unused — divider not installed in this variant) | — |
| 1 | I²C SDA | INA219 SDA (with 4.7kΩ pull-up to 3.3V) |
| 2 | I²C SCL | INA219 SCL (with 4.7kΩ pull-up to 3.3V) |
| 4 | Ultrasonic TRIG | AJ-SR04M TRIG |
| 5 | Ultrasonic ECHO | AJ-SR04M ECHO |
| 7 | WS2812B data | WS2812B DIN |
| 8 | On-board LED | Built-in |
| 9 | Button | Tactile switch |
| 20 | UART1 RX | RYLR998 TXD |
| 21 | UART1 TX | RYLR998 RXD |
| 3, 6, 10 | **FREE** | Reserved |

#### Wire-by-wire — TX Variant B

Power chain (note INA219 inserted in series with battery+):

```
Solar panel (+)            →  CN3791 IN+
Solar panel (–)            →  CN3791 IN–
CN3791 BAT+                →  Junction X (see below)
CN3791 BAT–                →  Common GND

18650 BAT+ (post-protect)  →  INA219 V+ (battery side of shunt)
INA219 V–                  →  Junction X
Junction X                 →  CN3791 BAT+  AND  MT3608 VIN+
18650 BAT– (post-protect)  →  Common GND

MT3608 VOUT+               →  +5V rail — splits to:
                              (1) C3 SuperMini 5V (VBUS)
                              (2) WS2812B #1 VDD
                              (3) AJ-SR04M VCC
MT3608 VOUT–               →  Common GND
MT3608 EN (optional)       →  C3 SuperMini GPIO10 (recommended for sleep gating)
```

INA219 control wires:

```
C3 SuperMini GPIO1         →  INA219 SDA  (4.7kΩ pull-up to 3.3V — most modules include this)
C3 SuperMini GPIO2         →  INA219 SCL  (4.7kΩ pull-up to 3.3V — same)
C3 SuperMini 3.3V (output) →  INA219 VS  (powers the IC; ≠ load voltage)
C3 SuperMini GND           →  INA219 GND
```

Notes on INA219 wiring:
- **V+ goes to the battery side of the shunt**, V– goes to the common-node side. The chip's bus-voltage register reads V– (= load voltage = battery voltage minus tiny shunt drop). The shunt-voltage register is signed: positive when current flows from V+ to V– (discharging), negative when reversed (charging).
- Address jumpers A0/A1 left **open** → I²C address `0x40` (default, what firmware probes).
- Module is wired in series with **battery+** (high-side sensing). Don't accidentally place it between the panel and the charger — it must see both charge and discharge currents in opposite directions.

Signal/sensor wires (identical to Variant A — same firmware binary):

```
C3 SuperMini GPIO4         →  AJ-SR04M TRIG
C3 SuperMini GPIO5         →  AJ-SR04M ECHO
C3 SuperMini GPIO7         →  WS2812B #1 DIN
WS2812B #1 DOUT            →  WS2812B #2 DIN  (chain)
C3 SuperMini GPIO9         →  Tactile button → GND
C3 SuperMini GPIO20 (RX1)  →  RYLR998 TXD
C3 SuperMini GPIO21 (TX1)  →  RYLR998 RXD
C3 SuperMini 3.3V (output) →  RYLR998 VCC
C3 SuperMini GND           →  Common GND
```

### INA219 wiring detail (Variant B)

The INA219 is wired with its shunt resistor **in series with the battery's positive terminal**, so a single shunt sees both charge and discharge currents. INA219's signed shunt-voltage register distinguishes the direction automatically.

```
  [Battery+] ──[INA219 SHUNT (R_SH)]── [Common+ node] ─┬─ CN3791 BAT pin
                  │  │                                  └─ MT3608 boost input
                 V+  V-
                  ▲   ▲
                  │   │
       INA219 V+ pin connects to battery side of shunt
       INA219 V- pin connects to common-node side of shunt

  INA219 SDA  → ESP32-C3 GPIO1
  INA219 SCL  → ESP32-C3 GPIO2
  INA219 VS   → ESP32-C3 3.3V output pin (powers the IC; ≠ load voltage)
  INA219 GND  → common ground
  INA219 ADDR → 0x40 (default; A0/A1 jumpers left open)
```

**Sign convention** (firmware-level interpretation):
- Positive shunt current → battery is **discharging** (current leaving battery toward boost converter)
- Negative shunt current → battery is **charging** (current flowing from CN3791 into battery)

Bus voltage register reads the V- side relative to GND, which is the battery voltage (post-shunt; the shunt drop is millivolts and negligible for SoC display).

### PCB-specific notes (required for board layout, optional on breadboard)

These are issues that **breadboard prototyping forgives but a production PCB does not**. Capture them in the schematic before ordering boards.

1. **ECHO line voltage divider — MANDATORY on PCB.** AJ-SR04M's ECHO output is 5V; ESP32-C3 GPIO inputs are 3.3V tolerant only (max VDD+0.3V = 3.6V absolute). Sustained 5V on GPIO5 stresses the I/O over weeks. Add **1kΩ in series + 2kΩ to GND** between AJ-SR04M ECHO and C3 GPIO5 (drops 5V → ~3.3V). On breadboard you might get away without it for hours of testing; on a PCB it's not optional.
2. **TRIG line is fine at 3.3V.** The C3's GPIO4 outputs 3.3V, the AJ-SR04M's TRIG input accepts >2.4V as logic HIGH. No level shifting needed for the outbound direction.
3. **INA219 module pull-ups (Variant B).** Most pre-built INA219 modules (Adafruit, Robu, generic AliExpress) include 4.7kΩ I²C pull-ups onboard. If you're populating a discrete INA219 chip directly on the TX PCB (no module), you MUST add pull-ups manually: 4.7kΩ from SDA→3V3 and SCL→3V3.
4. **Decoupling on every IC.** 100nF ceramic between VCC↔GND placed within 5mm of each IC's power pin: ESP32-C3, INA219, RYLR998. PCB layout discipline.
5. **Antenna keep-out for RYLR998.** The RYLR998's antenna trace needs **>5mm clear of any copper pour, ground plane, or metal component**. Document as a layer note in KiCad/EasyEDA. Same applies to the C3 SuperMini's onboard PCB antenna — the C3 module should sit at the *edge* of the board with its antenna end facing outward.
6. **18650 reverse-polarity protection.** Add a Schottky diode (e.g. SS34) inline with the battery+ lead before CN3791 BAT+. Cheap insurance against a battery inserted backwards.
7. **Status LED choice on GPIO8.** Firmware controls the C3 SuperMini's onboard built-in LED on GPIO8. On a custom PCB, you have two options: (a) keep the C3 module's onboard LED visible through the enclosure, or (b) add a discrete LED with a 330Ω current-limit resistor from GPIO8 to a panel-mounted indicator. Option (b) gives a brighter, larger indicator.
8. **Optional WS2812 chain on GPIO7.** The 2-LED WS2812 chain on GPIO7 is firmware-supported but **optional** on a TX PCB if the onboard LED on GPIO8 is enough visual feedback. Drop GPIO7 + the WS2812 components from the BOM if you want a simpler/cheaper TX board; firmware will handle the missing chain gracefully.

### Sensor auto-detect & manual override

Firmware behavior at boot:

1. Initialize I²C bus on GPIO1 (SDA) / GPIO2 (SCL) at 100 kHz
2. Probe address `0x40` with a single read
3. If ACK → `power_mode = "ina219"`, save to NVS (overwrites manual setting only if `manual_override = "auto"`)
4. If NACK → `power_mode = "voltage"`, fall back to ADC on GPIO0 with the existing `battery_monitor` divider logic
5. NVS-stored `power_mode_override` field can force any mode: `"auto"`, `"ina219"`, `"voltage"`, `"disabled"`

Web UI exposes the override under TX Settings → Power Monitoring with a `<select>` dropdown showing the auto-detected value. PWA reads `/api/system → power_mode` and conditionally renders basic vs rich power telemetry.

---

## Bill of Materials (BOM additions)

For the canonical full BOM see `BOM.csv` in this directory. New entries added 2026-04-27 to support the solar-TX power chain:

| Component | Variant | Approx ₹ (Robu / generic) | Notes |
|---|---|---|---|
| Solar panel 6V / 180mA crystalline | A & B | ~150 | Polycrystalline, ~250×80 mm |
| CN3791 MPPT solar charger module | A & B | ~120 | Buck-MPPT with R_PROG charge-current setting |
| 18650 Li-ion 3000 mAh + protected holder | A & B | ~200 | Protection circuit mandatory (CN3791 lacks over-discharge cutoff) |
| MT3608 DC-DC boost module 3.7V→5V | A & B | ~50 | EN pin recommended to GPIO10 for deep-sleep gating |
| 100 kΩ resistor ×2 (voltage divider) | A only | ~5 | 1% tolerance recommended for accurate SoC |
| INA219 module (Adafruit-style, 0.1Ω shunt) | B only | ~120 | Default I²C address 0x40 |
| 4.7 kΩ resistor ×2 (I²C pull-ups) | B only | ~5 | Most INA219 modules already include these onboard |

**Variant A total adder:** ~₹525
**Variant B total adder:** ~₹640

---

## Future expansion (reserved free GPIOs on TX)

| GPIO | Reserved for | Why |
|---|---|---|
| GPIO3 | CN3791 STAT pin (charging-LED output) | Direct read of charger state without inferring from current sign |
| GPIO6 | Future PIR / motion sensor or tank temperature sensor | Wake-on-motion to extend battery; or 1-Wire DS18B20 for water-temperature telemetry (see `project_tx_temp_sensor_planned.md` in agent memory) |
| GPIO10 | MT3608 boost EN pin | Disable boost in deep-sleep to recover ~120 mAh/day quiescent loss |

## Future expansion — RX

| GPIO | Reserved for | Why |
|---|---|---|
| GPIO34–39 | Input-only sensors (light, temperature, humidity for room-context insights) | Hub-side environmental sensing — e.g. ambient light to dim OLED at night automatically |
| GPIO25, 26 | DAC outputs for piezo buzzer | Audible alert when tank reaches critical low (separate from push notification) |
| GPIO32, 33 | Additional panel buttons if firmware grows beyond pair/AP-mode | E.g. "Identify Hub" (flash LEDs to confirm physical unit), "Force MQTT reconnect" |

---

## Firmware compatibility

The shared `battery_monitor` component (in both `public/firmware/transmitter/components/battery_monitor/` and `cloud/firmware/Transmitter-IDF/components/battery_monitor/`) currently provides voltage-divider support. INA219 + auto-detect support is added in **both trees** as a hardware-platform feature (not a cloud-roadmap feature) — see `project_firmware_strategy.md` rule #2 in project memory.

After the firmware update lands, the API surface becomes:

```c
// power_monitor.h (was battery_monitor.h)

typedef enum {
    POWER_MODE_NONE,
    POWER_MODE_VOLTAGE,
    POWER_MODE_INA219,
} power_mode_t;

typedef struct {
    power_mode_t mode;
    int      pct;          // 0–100 estimated SoC
    uint32_t vbat_mv;      // battery voltage
    int32_t  current_ma;   // signed; +ve = discharging, -ve = charging (INA219 only; 0 in voltage mode)
    int32_t  power_mw;     // V × I (INA219 only; 0 in voltage mode)
    bool     charging;     // explicit flag (from current sign or voltage trend)
} power_reading_t;

esp_err_t power_init(void);                      // auto-detect + NVS-stored override
esp_err_t power_read(power_reading_t *out);
esp_err_t power_set_override(const char *mode);  // "auto" | "ina219" | "voltage" | "disabled"
```

---

## License

Hardware design is open: BOM under CC BY-SA 4.0, schematics & wiring docs (this file) under MIT. Firmware in `public/firmware/` is MIT; firmware in `cloud/firmware/` is AGPL-3.0 (proprietary cloud variant).
