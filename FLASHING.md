# Flashing Pre-compiled Firmware - Web Flasher Method

This guide shows you how to flash the pre-compiled `.bin` files to your ESP32-C3 using your web browser. **No command-line tools or software installation required!**

## 📥 Download Firmware

Download the latest firmware from the [Releases](https://github.com/Techposts/LoRa-Water-Tank-Monitor/releases) page or from the `firmware/` directory:

- **Transmitter**: `Transmitter_ESP32C3_v1.0.bin` (4MB)
- **Receiver**: `Receiver_ESP32C3_v1.0.bin` (4MB)

---

## 🌐 Web Flasher (Browser-based) - RECOMMENDED

### Requirements

- **Browser**: Chrome, Edge, or Opera (WebSerial support required)
- **USB Cable**: Data cable (not charge-only)
- **Driver**: CH340 or CP2102 USB driver (usually auto-installed)

### Step-by-Step Instructions

#### Step 1: Visit ESP Web Tool

Open this website in Chrome, Edge, or Opera:

🔗 **https://espressif.github.io/esptool-js/**

#### Step 2: Connect ESP32-C3

1. Connect your ESP32-C3 SuperMini to your computer via USB
2. The device should be recognized automatically

#### Step 3: Select Device

Click **"Connect"** button on the website

![Selecting the board](docs/Selecting%20the%20board.jpg)

A popup will appear showing available serial ports:
- Select your ESP32-C3 device (usually shows as "USB Serial" or "CH340")
- Click **"Connect"**

#### Step 4: Program the Firmware

![Programming and flashing the bin file](docs/Programing%20and%20flashing%20the%20bin%20file.jpg)

1. **Choose File**: Click and select your downloaded `.bin` file
   - For Transmitter: `Transmitter_ESP32C3_v1.0.bin`
   - For Receiver: `Receiver_ESP32C3_v1.0.bin`

2. **Flash Address**: Enter `0x0` (zero-x-zero)
   - This is VERY IMPORTANT - must be `0x0`

3. **Flash Mode**: Leave as default (DIO)

4. **Flash Speed**: Leave as default (40MHz or higher)

5. Click **"Program"**

#### Step 5: Wait for Completion

- Progress bar will show flashing status
- Takes about 30-60 seconds
- Wait for "Success" message
- **Do NOT disconnect** during flashing!

#### Step 6: Reset Device

After flashing completes:
1. Disconnect USB cable
2. Wait 2 seconds
3. Reconnect USB cable
4. Device will boot with new firmware

---

## ✅ Verify Flash Success

### Transmitter Verification

**LED Indicators**:
- 🟡 Yellow: Starting up
- 🔵 Blue: Taking measurements
- 🔵 Cyan: Transmitting
- 🟢 Green (3 blinks): Success!

**Serial Monitor** (optional):
If you have serial monitor:
```
╔════════════════════════════════════════════════════════════╗
║       LoRa Water Tank Monitor - TRANSMITTER                ║
╠════════════════════════════════════════════════════════════╣
║  Frequency: 865000000 Hz                                   ║
║  Network ID: 6    TX Power: 14 dBm                         ║
║  My Address: 1    Receiver: 2                              ║
║  Sleep Time: 5 minutes                                     ║
╚════════════════════════════════════════════════════════════╝
```

### Receiver Verification

**OLED Display** should show:
```
┌─────────────┐
│  TankSync   │
│ Initializing│
│             │
│ AP: 192...  │
└─────────────┘
```

**LED Indicators**:
- Both LEDs pulse cyan briefly during boot
- LED 1 (Status): Orange during initialization
- LED 1 (Status): Green after LoRa connects
- LED 0 (Water): Orange blinking (waiting for data)

**WiFi AP Created**:
- Look for WiFi network: **"TankSync"** (no password)

---

## ⚙️ Post-Flash Configuration

### Transmitter Configuration

The transmitter uses **default settings**:

```
LORA_FREQUENCY:      865000000 (865 MHz - India)
LORA_NETWORK_ID:     6
MY_ADDRESS:          1
RECEIVER_ADDRESS:    2
SLEEP_MINUTES:       5
TX_POWER:            14 dBm
```

**Note**: Settings are hardcoded. To change, you need to edit source code and recompile.

### Receiver Configuration

Configure via **web interface**:

#### First Boot Setup:

1. **Connect to WiFi AP**:
   - WiFi Network: `TankSync`
   - Password: None (open network)

2. **Open Configuration Page**:
   - Navigate to: `http://192.168.4.1/config`

3. **Configure Settings**:
   - **WiFi**: Your SSID and password
   - **MQTT**: Broker IP, port, credentials (optional)
   - **Tank**: Min/max distance, capacity
   - **LoRa**: Frequency, network ID (must match transmitter)

4. **Save and Reboot**:
   - Click "Save"
   - Device will restart and connect to your WiFi

5. **Access Dashboard**:
   - Find receiver IP on OLED display
   - Navigate to: `http://<receiver-ip>/`

---

## 🔧 Troubleshooting

### "No compatible devices found" or Can't Connect

**Solutions**:

1. **Wrong Browser**:
   - Use Chrome, Edge, or Opera
   - Safari and Firefox don't support WebSerial

2. **USB Driver Missing**:
   - Windows: Install [CH340 driver](http://www.wch-ic.com/downloads/CH341SER_EXE.html)
   - macOS: Usually works without driver
   - Linux: Usually works without driver

3. **Bad USB Cable**:
   - Use a DATA cable, not charge-only
   - Try a different cable

4. **USB Port Issue**:
   - Try different USB port
   - Avoid USB hubs - connect directly to computer

### Flash Failed or Error During Programming

**Solutions**:

1. **Put Device in Bootloader Mode**:
   - Disconnect USB
   - Hold BOOT button (if available)
   - Connect USB while holding BOOT
   - Release BOOT
   - Try flashing again

2. **Lower Flash Speed**:
   - In web tool, try lower baud rate
   - Try 115200 instead of 921600

3. **Full Erase First**:
   - Click "Erase" button in web tool
   - Wait for completion
   - Then flash firmware

### Device Not Working After Flash

**Solutions**:

1. **Wrong Firmware**:
   - Make sure you flashed correct file
   - Transmitter.bin → Tank unit
   - Receiver.bin → Indoor unit

2. **Power Cycle**:
   - Disconnect USB
   - Wait 5 seconds
   - Reconnect USB

3. **Check Connections**:
   - Verify LoRa module connections
   - Check sensor connections (transmitter)
   - Check OLED connections (receiver)

### Transmitter Not Sending Data

**Check**:
- ✅ LoRa module connected (GPIO 20, 21)
- ✅ Sensor connected (GPIO 4, 5)
- ✅ Battery connected and charged
- ✅ Antenna attached to LoRa module

**LED Pattern**:
- If red blinks 10 times: LoRa initialization failed
- If red blinks 5 times: Transmission failed (no receiver)

### Receiver Not Receiving Data

**Check**:
- ✅ LoRa module connected (GPIO 20, 21)
- ✅ LoRa settings match transmitter
- ✅ Both devices on same frequency (865 MHz)
- ✅ Both devices on same network ID (6)
- ✅ Antenna attached to LoRa module

**LED Pattern**:
- Red solid: LoRa hardware error
- Blue blinking: AP mode, waiting for WiFi config
- Cyan blinking: Waiting for first data

---

## 🔄 Re-flashing or Updating

To flash new firmware or switch between transmitter/receiver:

1. Simply repeat the flashing process
2. No need to erase first (unless having issues)
3. New firmware will completely replace old firmware

---

## 📊 Default Firmware Configuration

### Both Devices

| Setting | Value | Notes |
|---------|-------|-------|
| Board | ESP32-C3 | SuperMini or Dev Module |
| LoRa Frequency | 865 MHz | For India (change if needed) |
| Network ID | 6 | Must match on both devices |
| Baud Rate | 115200 | Serial monitor |

### Transmitter Only

| Setting | Value |
|---------|-------|
| Device Address | 1 |
| Receiver Address | 2 |
| Sleep Interval | 5 minutes |
| TX Power | 14 dBm |

### Receiver Only

| Setting | Value |
|---------|-------|
| Device Address | 2 |
| WiFi AP SSID | TankSync |
| WiFi AP Password | None (open) |
| MQTT Server | 192.168.0.163 |
| MQTT Port | 1885 |

---

## 🎯 Quick Reference

### Transmitter Flash

1. Open: https://espressif.github.io/esptool-js/
2. Connect ESP32-C3
3. Click "Connect" → Select device
4. Choose `Transmitter_ESP32C3_v1.0.bin`
5. Address: `0x0`
6. Click "Program"
7. Wait for success
8. Reconnect USB

### Receiver Flash

1. Open: https://espressif.github.io/esptool-js/
2. Connect ESP32-C3
3. Click "Connect" → Select device
4. Choose `Receiver_ESP32C3_v1.0.bin`
5. Address: `0x0`
6. Click "Program"
7. Wait for success
8. Reconnect USB
9. Configure WiFi at http://192.168.4.1/config

---

## 🆘 Still Having Issues?

1. **Check Documentation**:
   - [Main README](README.md) - Hardware setup
   - [Transmitter README](Transmitter/README.md) - Transmitter details
   - [Receiver README](Receiver/README.md) - Receiver details

2. **Search Issues**:
   - [GitHub Issues](https://github.com/Techposts/LoRa-Water-Tank-Monitor/issues)

3. **Ask for Help**:
   - Open new issue with:
     - Browser and OS version
     - Complete error message
     - Photos of hardware connections
     - Serial monitor output (if available)

---

## 📱 Alternative: Command Line Method

If web flasher doesn't work, you can use esptool command line:

```bash
# Install esptool
pip install esptool

# Flash command
esptool.py --chip esp32c3 --port COM3 --baud 921600 write_flash 0x0 firmware.bin
```

Replace `COM3` with your port (Windows: COM3, Linux: /dev/ttyUSB0, macOS: /dev/cu.usbserial-*)

---

**Happy Flashing! 💾**

For complete hardware assembly and setup guide, see [README.md](README.md)
