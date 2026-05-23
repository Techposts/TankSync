# TankSync Receiver — ESP32 DevKit v1 (CP2102)

## Board: ESP32-WROOM-32 DevKit v1
- **Chip**: ESP32 dual-core Xtensa LX6 @ 240MHz
- **Flash**: 4MB
- **RAM**: ~320KB SRAM
- **USB**: CP2102 USB-Serial
- **Cores**: 2 (eliminates single-core UART/I2C contention issues from ESP32-C3)

## Pin Mapping

| Function | GPIO | Notes |
|----------|------|-------|
| LoRa TX (→ RYLR998 RXD) | 17 | UART2 TX |
| LoRa RX (← RYLR998 TXD) | 16 | UART2 RX |
| WS2812B LED Data | 13 | GPIO2 is onboard LED, avoid |
| OLED SDA | 21 | Standard I2C |
| OLED SCL | 22 | Standard I2C |
| Buzzer (active) | 2 | Shares onboard blue LED — LED pulses with buzzer. **Strapping pin: disconnect buzzer wire before USB flashing** (held-low at boot blocks ROM bootloader). OTA flashing unaffected. |

### Buzzer wiring

Active 3-pin buzzer module (the kind sold as "active buzzer module" — has on-board oscillator):

```
ESP32 DevKit              Buzzer module
------------              -------------
GPIO2        ------>      I/O  (signal)
3.3V         ------>      VCC
GND          ------>      GND
```

If you use a passive buzzer instead (no on-board oscillator), the firmware's
plain GPIO toggle won't drive it — passive buzzers need PWM at the resonant
frequency (typically 2 kHz). Swap `buzzer.c::buzz_on/off()` to use LEDC if needed.

**Flashing caveat**: GPIO2 is a strapping pin. If the buzzer holds the line
low at boot, the chip enters download mode and won't run firmware. For USB
flashing: disconnect the buzzer signal wire from GPIO2 first, then reconnect
after flash completes. OTA (over-WiFi) flashing is unaffected — the chip
re-reads strapping pins only at hard reset, not on OTA reboot.

## Wiring: RYLR998 LoRa Module

```
ESP32 DevKit          RYLR998
-----------          --------
GPIO17 (TX2) ------> RXD
GPIO16 (RX2) <------ TXD
3.3V         ------> VDD
GND          ------> GND
```

## Wiring: SH1106 OLED (optional)

```
ESP32 DevKit          SH1106 OLED
-----------          -----------
GPIO21 (SDA) ------> SDA
GPIO22 (SCL) ------> SCL
3.3V         ------> VCC
GND          ------> GND
```

## Wiring: WS2812B LEDs (optional)

```
ESP32 DevKit          WS2812B
-----------          -------
GPIO13       ------> DIN
5V           ------> VCC
GND          ------> GND
```

## Build & Flash

```bash
cd "Receiver-ESP32-DevKit"
export IDF_PATH=/path/to/esp-idf
. $IDF_PATH/export.sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/cu.SLAB_USBtoUART flash monitor
```

## Key Differences from ESP32-C3 Build

| Feature | ESP32-C3 (Receiver-IDF) | ESP32 DevKit (this) |
|---------|------------------------|---------------------|
| Cores | 1 (RISC-V) | 2 (Xtensa LX6) |
| UART for LoRa | UART1 (GPIO20/21) | UART2 (GPIO16/17) |
| I2C pins | GPIO9/10 | GPIO21/22 |
| INT WDT | Disabled (systimer bug) | Enabled (300ms) |
| CPU freq | 160MHz | 240MHz |
| USB | USB-Serial/JTAG (native) | CP2102 (external) |
| Serial port | /dev/cu.usbmodemXXXXX | /dev/cu.SLAB_USBtoUART |

## Why ESP32 DevKit?

The ESP32-C3 rev 0.4 has hardware issues:
1. `systimer_ll_is_counter_value_valid` busy-loops during interrupts → INT WDT crash
2. Single-core means UART/I2C/WiFi all compete for CPU → livelocks during OTA
3. I2C bus recovery can lock the APB bus → hard freeze

The ESP32 dual-core eliminates all three issues.
