# Pre-compiled Firmware Releases

This directory contains ready-to-flash `.bin` files for the LoRa Water Tank Monitor.

## 📥 Quick Download

### Latest Release: v1.0

| Component | Download | Size | Description |
|-----------|----------|------|-------------|
| **Transmitter** | [v1.0/Transmitter_ESP32C3_v1.0.bin](v1.0/Transmitter_ESP32C3_v1.0.bin) | 4.0 MB | Tank-side battery-powered unit |
| **Receiver** | [v1.0/Receiver_ESP32C3_v1.0.bin](v1.0/Receiver_ESP32C3_v1.0.bin) | 4.0 MB | Indoor USB-powered unit |

### Release Notes

See [v1.0/RELEASE_NOTES.md](v1.0/RELEASE_NOTES.md) for full details.

---

## 🚀 Quick Flash

### Web Flasher (Easiest - Recommended)

**No software installation required!**

1. Open in Chrome/Edge/Opera: **https://espressif.github.io/esptool-js/**
2. Connect ESP32-C3 via USB
3. Click "Connect" and select your device
4. Choose the `.bin` file
5. Set address to `0x0`
6. Click "Program"

**Detailed instructions**: See [FLASHING.md](../FLASHING.md) with screenshots

### Command Line (Alternative)

Install [esptool](https://github.com/espressif/esptool):
```bash
pip install esptool
```

**Transmitter**:
```bash
esptool.py --chip esp32c3 --port COM3 --baud 921600 write_flash 0x0 v1.0/Transmitter_ESP32C3_v1.0.bin
```

**Receiver**:
```bash
esptool.py --chip esp32c3 --port COM3 --baud 921600 write_flash 0x0 v1.0/Receiver_ESP32C3_v1.0.bin
```

**Note**: Replace `COM3` with your actual port:
- Windows: `COM3`, `COM4`, etc.
- Linux: `/dev/ttyUSB0`, `/dev/ttyACM0`
- macOS: `/dev/cu.usbserial-*`

---

## 📖 Complete Flashing Guide

For detailed instructions including:
- Windows GUI tools (Flash Download Tool)
- Web-based flasher
- Troubleshooting
- Post-flash configuration

See [FLASHING.md](../FLASHING.md)

---

## 🔍 What's in Each Binary?

Each `.bin` file is a **merged binary** containing:
- ✅ ESP32-C3 bootloader
- ✅ Partition table
- ✅ Complete application code
- ✅ All required libraries

**No compilation needed** - just flash and go!

---

## ⚙️ Default Configuration

### Transmitter
```
LoRa Frequency:    865 MHz (India)
Network ID:        6
Device Address:    1
Receiver Address:  2
Sleep Interval:    5 minutes
TX Power:          14 dBm
```

**To change**: Must edit code and recompile (OTA update coming in v2.0)

### Receiver
```
LoRa Frequency:    865 MHz
Network ID:        6
Device Address:    2
WiFi AP:           TankSync (no password)
MQTT Server:       192.168.0.163:1885
```

**To change**: Use web interface at `http://192.168.4.1/config`

---

## 🎯 Board Compatibility

These binaries are compiled for:
- **ESP32-C3 SuperMini** (recommended)
- Generic ESP32-C3 Dev Module
- XIAO ESP32-C3 (should work)
- Other ESP32-C3 boards (may work)

**Not compatible with**:
- ESP32 (original)
- ESP32-S2
- ESP32-S3
- ESP8266

---

## 📂 Directory Structure

```
firmware/
├── README.md (this file)
└── v1.0/
    ├── RELEASE_NOTES.md
    ├── Transmitter_ESP32C3_v1.0.bin
    └── Receiver_ESP32C3_v1.0.bin
```

---

## 🔄 Version History

| Version | Date | Transmitter | Receiver | Notes |
|---------|------|-------------|----------|-------|
| **v1.0** | Jan 2025 | 4.0 MB | 4.0 MB | Initial stable release |

---

## 🛡️ Verification

### MD5 Checksums (Coming Soon)

To verify file integrity:
```bash
# Linux/macOS
md5sum v1.0/*.bin

# Windows
certutil -hashfile v1.0/Transmitter_ESP32C3_v1.0.bin MD5
```

---

## 🐛 Having Issues?

1. **Read**: [FLASHING.md](../FLASHING.md) troubleshooting section
2. **Check**: [GitHub Issues](https://github.com/Techposts/LoRa-Water-Tank-Monitor/issues)
3. **Ask**: [GitHub Discussions](https://github.com/Techposts/LoRa-Water-Tank-Monitor/discussions)

---

## 📜 License

These binaries are provided under the MIT License, same as the source code.

**Free to use for personal and commercial projects.**

---

**Happy Flashing! 💾**

For hardware assembly guide, see [main README](../README.md)
