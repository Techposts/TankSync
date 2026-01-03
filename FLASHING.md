# Flashing Pre-compiled Firmware

This guide explains how to flash the pre-compiled `.bin` files to your ESP32-C3 without needing to compile the code yourself.

## 📥 Download Firmware

Download the latest firmware from the [Releases](https://github.com/Techposts/LoRa-Water-Tank-Monitor/releases) page or from the `firmware/` directory:

- **Transmitter**: `Transmitter_ESP32C3_v1.0.bin` (4MB)
- **Receiver**: `Receiver_ESP32C3_v1.0.bin` (4MB)

## 🛠 Flashing Methods

Choose one of the following methods to flash the firmware:

---

## Method 1: ESPTool (Command Line) - Recommended

### Windows / Linux / macOS

#### 1. Install ESPTool

**Using pip (Python)**:
```bash
pip install esptool
```

Or download from: https://github.com/espressif/esptool

#### 2. Connect ESP32-C3

- Connect ESP32-C3 SuperMini to your computer via USB
- Put device in bootloader mode (usually automatic on ESP32-C3)
- Note the COM port:
  - **Windows**: Check Device Manager (e.g., `COM3`)
  - **Linux**: Usually `/dev/ttyUSB0` or `/dev/ttyACM0`
  - **macOS**: Usually `/dev/cu.usbserial-*`

#### 3. Flash Transmitter

```bash
# Windows
esptool.py --chip esp32c3 --port COM3 --baud 921600 write_flash 0x0 Transmitter_ESP32C3_v1.0.bin

# Linux/macOS
esptool.py --chip esp32c3 --port /dev/ttyUSB0 --baud 921600 write_flash 0x0 Transmitter_ESP32C3_v1.0.bin
```

#### 4. Flash Receiver

```bash
# Windows
esptool.py --chip esp32c3 --port COM3 --baud 921600 write_flash 0x0 Receiver_ESP32C3_v1.0.bin

# Linux/macOS
esptool.py --chip esp32c3 --port /dev/ttyUSB0 --baud 921600 write_flash 0x0 Receiver_ESP32C3_v1.0.bin
```

#### 5. Verify Flash

After flashing, open serial monitor at 115200 baud to verify:
```bash
# Windows
python -m serial.tools.miniterm COM3 115200

# Linux/macOS
python -m serial.tools.miniterm /dev/ttyUSB0 115200
```

Press the RESET button on ESP32-C3 and you should see boot messages.

---

## Method 2: ESP Flash Download Tool (Windows GUI)

### Download Tool

1. Download **Flash Download Tools** from Espressif:
   https://www.espressif.com/en/support/download/other-tools

2. Extract and run `flash_download_tool_x.x.x.exe`

### Flash Steps

1. **Select Chip**: Choose `ESP32-C3`
2. **Select Mode**: Choose `Developer Mode`
3. **Configure Download**:
   - Check the checkbox next to file selection
   - Click `...` and select the `.bin` file
   - Set address to `0x0`

   ```
   [✓] Transmitter_ESP32C3_v1.0.bin @ 0x0
   ```

4. **COM Port Settings**:
   - **COM**: Select your COM port (e.g., COM3)
   - **BAUD**: 921600

5. **Click START**

6. Wait for "FINISH" message

7. Press RESET button on ESP32-C3

### Screenshot Reference

```
┌─────────────────────────────────────────┐
│  Flash Download Tool                    │
├─────────────────────────────────────────┤
│  [✓] firmware.bin    @ 0x0     [...]    │
│  [ ]                 @         [...]    │
│                                          │
│  SPI SPEED: 80MHz  ▼                    │
│  SPI MODE:  DIO    ▼                    │
│  COM PORT:  COM3   ▼   BAUD: 921600 ▼  │
│                                          │
│            [ START ]  [ STOP ]          │
└─────────────────────────────────────────┘
```

---

## Method 3: Web Flasher (Browser-based)

### ESP Web Tools

1. Visit: https://espressif.github.io/esptool-js/

2. Click **Connect**

3. Select your ESP32-C3 COM port

4. Click **Choose File** and select the `.bin` file

5. Set **Flash Address**: `0x0`

6. Click **Program**

7. Wait for completion

**Note**: Requires Chrome, Edge, or Opera browser (WebSerial support needed)

---

## Method 4: Arduino IDE (Alternative)

If you have Arduino IDE installed but don't want to compile:

### Arduino IDE 2.x

1. Open Arduino IDE
2. Go to `Sketch` → `Upload Using Programmer`
3. This won't work for pre-compiled binaries

**For pre-compiled binaries, use ESPTool or Flash Download Tool instead.**

---

## ⚙️ Post-Flash Configuration

### Transmitter Configuration

The transmitter uses **default settings** after flashing:

```cpp
LORA_FREQUENCY:      865000000 (865 MHz - India)
LORA_NETWORK_ID:     6
MY_ADDRESS:          1
RECEIVER_ADDRESS:    2
SLEEP_MINUTES:       5
```

**To change settings**:
- You must edit `Transmitter.ino` and recompile, OR
- Wait for future OTA update feature

### Receiver Configuration

The receiver can be configured via **web interface**:

1. **First Boot**:
   - Receiver creates WiFi AP: `TankSync` (no password)
   - Connect to `TankSync`
   - Navigate to `http://192.168.4.1/config`

2. **Configure**:
   - WiFi SSID and Password
   - MQTT broker settings (optional)
   - Tank calibration (min/max distance, capacity)
   - LoRa settings (must match transmitter)

3. **Save and Reboot**

4. **Access Dashboard**:
   - After reboot, find receiver IP from OLED display or serial monitor
   - Navigate to `http://<receiver-ip>/`

---

## 🔍 Troubleshooting

### Flash Failed: "A fatal error occurred: Failed to connect"

**Solutions**:
1. **Check USB Cable**: Use a data cable, not charge-only
2. **Driver**: Install CH340 or CP2102 USB driver
3. **Bootloader Mode**:
   - Disconnect USB
   - Hold BOOT button
   - Connect USB while holding BOOT
   - Release BOOT button
   - Try flashing again

### Flash Failed: "A fatal error occurred: Timed out waiting for packet header"

**Solutions**:
1. **Lower Baud Rate**: Try `460800` or `115200` instead of `921600`
2. **Shorter Cable**: Use a shorter, high-quality USB cable
3. **Different Port**: Try a different USB port on your computer

### Flash Successful but Device Not Working

**Solutions**:
1. **Check Serial Monitor**: Open at 115200 baud and look for errors
2. **Full Erase**: Erase flash first, then reflash
   ```bash
   esptool.py --chip esp32c3 --port COM3 erase_flash
   esptool.py --chip esp32c3 --port COM3 --baud 921600 write_flash 0x0 firmware.bin
   ```
3. **Power Cycle**: Disconnect USB, wait 5 seconds, reconnect

### Wrong Firmware Flashed

If you flashed Receiver firmware to Transmitter (or vice versa):
- Simply flash the correct firmware
- No harm done, just reflash

---

## 📊 Verify Flash Success

### Transmitter Verification

Open serial monitor (115200 baud):

```
╔════════════════════════════════════════════════════════════╗
║       LoRa Water Tank Monitor - TRANSMITTER                ║
╠════════════════════════════════════════════════════════════╣
║  Frequency: 865000000 Hz                                   ║
║  Network ID: 6    TX Power: 14 dBm                         ║
║  My Address: 1    Receiver: 2                              ║
║  Sleep Time: 5 minutes                                     ║
╚════════════════════════════════════════════════════════════╝

▶ Initializing LoRa module...
  ✓ LoRa communication OK
  ✓ Address set to 1
  ✓ Network ID set to 6
...
```

### Receiver Verification

Open serial monitor (115200 baud):

```
=== TankSync Receiver - ESP32-C3 ===
WDT: Arduino framework (yield every ~20ms)

WiFi: Initializing...
LoRa: Initializing...
OLED: Display OK
LEDs: 2 LEDs initialized

READY | LEDs: 1=Status, 0=Water
```

**OLED Display** should show:
```
┌─────────────┐
│  TankSync   │
│ Initializing│
│             │
│ AP: 192...  │
└─────────────┘
```

---

## 🔧 Advanced: Partial Flash (For Updates)

If you only want to update the application (not bootloader):

### Flash Only Application

```bash
# Application starts at 0x10000
esptool.py --chip esp32c3 --port COM3 --baud 921600 write_flash 0x10000 app.bin
```

**Note**: Use this only if you know what you're doing. For normal flashing, use `0x0` with the merged binary.

---

## 📦 File Information

### Firmware Files Explained

| File | Size | Description | Flash Address |
|------|------|-------------|---------------|
| **firmware.merged.bin** | 4 MB | Complete firmware (bootloader + app + partitions) | `0x0` |
| firmware.bin | ~1 MB | Application only | `0x10000` |
| firmware.bootloader.bin | 20 KB | Bootloader only | `0x0` |
| firmware.partitions.bin | 3 KB | Partition table | `0x8000` |

**For easy flashing, always use the `.merged.bin` file at address `0x0`.**

---

## 🆘 Getting Help

If you encounter issues:

1. **Check Serial Monitor**: Most errors show helpful messages
2. **Search Issues**: Check [GitHub Issues](https://github.com/Techposts/LoRa-Water-Tank-Monitor/issues)
3. **Open New Issue**: Provide:
   - Operating system
   - Flash command used
   - Complete error message
   - Serial monitor output

---

## 📋 Quick Reference

### Transmitter Flash Command

```bash
esptool.py --chip esp32c3 --port COM3 --baud 921600 write_flash 0x0 Transmitter_ESP32C3_v1.0.bin
```

### Receiver Flash Command

```bash
esptool.py --chip esp32c3 --port COM3 --baud 921600 write_flash 0x0 Receiver_ESP32C3_v1.0.bin
```

### Erase Flash (Factory Reset)

```bash
esptool.py --chip esp32c3 --port COM3 erase_flash
```

### Read Flash (Backup)

```bash
esptool.py --chip esp32c3 --port COM3 read_flash 0x0 0x400000 backup.bin
```

---

**Happy Flashing! 💾**

For hardware assembly and configuration, see [README.md](README.md)
