# TankSync Transmitter (ESP-IDF v2)

ESP32-C3 SuperMini battery-powered transmitter for the LoRa Water Tank Monitor.

## Hardware

- **MCU**: ESP32-C3 SuperMini (4MB flash, RISC-V single-core)
- **LoRa**: RYLR998 module (868/915 MHz)
- **Sensor**: AJ-SR04M ultrasonic distance sensor
- **Power**: LiPo battery with deep sleep (~300s cycle)

### Pin Configuration

| Function | GPIO | Notes |
|----------|------|-------|
| LoRa TX | 21 | UART1 TX to RYLR998 RXD |
| LoRa RX | 20 | UART1 RX from RYLR998 TXD |
| LED | 8 | On-board LED (C3 SuperMini) |
| Button | 9 | Boot button (active low, internal pullup) |
| Ultrasonic TRIG | 4 | AJ-SR04M trigger |
| Ultrasonic ECHO | 5 | AJ-SR04M echo |
| Battery ADC | 0 | Voltage divider (1:2 ratio) |

## Boot Window

On **power-on or reset** (NOT timer wake), the TX opens a 10-second boot window. LED blinks at 1Hz. Hold the button for:

| Hold Duration | Action | LED Feedback |
|---------------|--------|-------------|
| 2 seconds | **Pairing mode** — broadcasts PAIR_REQ via LoRa | 6 rapid flashes |
| 5 seconds | **Diagnostic mode** — prints system info to serial | Steady on |
| 8 seconds | **WiFi OTA mode** — creates WiFi AP for firmware update | 10 rapid flashes |

Timer wakes from deep sleep skip the boot window entirely (battery savings).

## Firmware Update Methods

### Method 1: WiFi AP OTA (Recommended for deployed devices)

1. Press **reset** on the transmitter
2. During the 10s boot window, **hold the button for 8 seconds**
3. LED flashes rapidly to confirm — TX creates WiFi AP: **`TankSync-Update`**
4. Connect your phone/laptop to the `TankSync-Update` WiFi network (open, no password)
5. Open a browser and go to **`http://192.168.4.1`**
6. The web page shows:
   - **Device Info** — current firmware version, LoRa address, sleep/sample settings
   - **Settings** — edit sleep interval and sample count, save directly to NVS
   - **Firmware Update** — select a `.bin` file and upload with progress bar
7. After successful upload, the TX verifies the binary and reboots
8. If no activity for 5 minutes, TX auto-reboots to normal mode

**Status**: WiFi AP starts and serves the page. OTA upload handler is implemented. Needs end-to-end testing.

### Method 2: LoRa OTA (Experimental — pushed from receiver)

The receiver can push firmware updates to the transmitter over LoRa during the TX's downlink window.

**Protocol**:
1. Receiver stages TX firmware binary on SPIFFS via web UI
2. When TX wakes and sends TANK data, receiver sends `OTA_START:<size>` (3x for reliability)
3. Both sides switch to SF7/500kHz for faster chunk streaming
4. TX sends `OTA_READY` (3x), enters chunk receive loop
5. Receiver streams 100-byte chunks as hex-encoded `OTA_DATA:<offset>:<hex>`
6. TX ACKs each chunk with `OTA_ACK:<next_offset>`
7. After all chunks: `OTA_END` / `OTA_DONE` exchange, TX reboots

**Status**: Partially working. Chunks are delivered at SF7/500kHz (~260ms per chunk, 23x faster than SF9/125kHz). However, ~40% ACK packet loss causes the transfer to stall after 6-30 chunks. The half-duplex nature of LoRa means the receiver can't receive ACKs while transmitting chunks.

**Known Issues**:
- ACK reliability: TX sends OTA_ACK but receiver misses ~40-50% due to half-duplex timing
- OTA_READY often lost (worked around with soft wait + proceed anyway)
- At SF9/125kHz, chunk airtime is 1050ms creating a large half-duplex blind window
- At SF7/500kHz, airtime drops to ~45ms but packet loss persists at lower rates

**Potential fixes** (contributions welcome):
- Blast mode: skip per-chunk ACK, stream all chunks with fixed delay, verify at end
- Adaptive retry with exponential backoff
- Reduce chunk size to minimize airtime
- Use address 0 (broadcast) for chunks to bypass address filtering edge cases

### Method 3: USB Serial (Development only)

```bash
cd Transmitter-IDF
. ~/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodem12201 flash
```

## Building

```bash
cd Transmitter-IDF
. ~/esp/esp-idf/export.sh    # or wherever your ESP-IDF is installed
idf.py set-target esp32c3
idf.py build
```

**Important**: After `set-target`, the sdkconfig may re-enable WiFi. The custom partition table and flash size are set in `sdkconfig.defaults`.

## Version Management

Two version strings must be kept in sync:
- `main/config.h` → `FIRMWARE_VERSION` (used in TANK packets and logs)
- `CMakeLists.txt` → `PROJECT_VER` (embedded in binary `esp_app_desc_t`, read by receiver for OTA display)

## Partition Table

```
nvs,      data, nvs,      0x9000,   0x5000
otadata,  data, ota,      0xe000,   0x2000
app0,     app,  ota_0,    0x10000,  0x1D0000  (1.8MB)
app1,     app,  ota_1,    0x1E0000, 0x1D0000  (1.8MB)
coredump, data, coredump, 0x3B0000, 0x10000
```

Current firmware size: ~860KB (with WiFi AP OTA) / ~263KB (without). Both fit within the 1.8MB partition.

## NVS Keys

| Namespace | Key | Type | Description |
|-----------|-----|------|-------------|
| `system` | `sleep_s` | u32 | Sleep interval in seconds (default: 300) |
| `system` | `samples` | u8 | Sensor samples per reading (default: 5) |
| `lora` | `my_addr` | u16 | Device LoRa address (set during pairing) |
| `lora` | `rx_addr` | u16 | Receiver's LoRa address (default: 2) |
| `lora` | `freq` | u32 | LoRa frequency in Hz |
| `lora` | `netid` | u8 | Network ID |
| `lora` | `sf` | u8 | Spreading factor (7-12) |
| `lora` | `bw` | u8 | Bandwidth (7=125kHz, 8=250kHz, 9=500kHz) |
| `lora` | `pwr` | u8 | TX power (0-22 dBm) |
