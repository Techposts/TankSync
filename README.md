# TankSync — reliable smart water monitoring

[![Pre-order Developer Edition](https://img.shields.io/badge/Pre--order-Developer%20Edition-success.svg?style=flat)](https://shop.smartghar.org)
[![Firmware: AGPL-3.0](https://img.shields.io/badge/Firmware-AGPL--3.0-blue.svg)](LICENSE)
[![Hardware: CC BY-SA 4.0](https://img.shields.io/badge/Hardware-CC%20BY--SA%204.0-orange.svg)](hardware/LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.4-red.svg)](https://docs.espressif.com/projects/esp-idf/)
[![Home Assistant](https://img.shields.io/badge/HA-Integration-blue.svg)](https://github.com/Techposts/smartghar-homeassistant)

**Never worry about your water tank again.** A solar-powered sensor on the rooftop, a quiet hub on the wall, and smart water monitoring that keeps working — even when the internet doesn't. Long-range LoRa (RYLR998) to an ESP32 hub, local web UI, Home Assistant via HACS, optional cloud PWA. Open at the core.

<p align="center">
  <img src="hardware/photos/render-hub-wall.png" width="32%" alt="TankSync hub — wall-mounted, live OLED showing 2 tanks, status LEDs for power, WiFi, and cloud" />
  <img src="hardware/photos/render-sensor-ultrasonic.png" width="32%" alt="TankSync solar tank sensor — side profile with ultrasonic measurement waves visualised" />
  <img src="hardware/photos/pcb-populated-top.jpg" width="32%" alt="Populated TX PCB — REV 2.2, May 2026 (open hardware)" />
</p>
<p align="center">
  <sub><em>The indoor hub (left), the solar tank sensor with non-contact ultrasonic measurement (centre), and the custom circular TX PCB (right) — current production hardware, REV 2.2 (May 2026), tested through Delhi summer at 45°C ambient.</em></sub>
</p>

## Watch the story — Episode 1

<p align="center">
  <a href="https://www.youtube.com/watch?v=ZZt6cZbWM0g">
    <img src="https://img.youtube.com/vi/ZZt6cZbWM0g/maxresdefault.jpg" width="70%" alt="TankSync Episode 1 — Smart Home That Works Without Internet" />
  </a>
</p>
<p align="center">
  <sub><em>Why I built TankSync, the local-first philosophy, and how the rooftop sensor + indoor hub stay reliable when the internet doesn't. <a href="https://www.youtube.com/watch?v=ZZt6cZbWM0g">Watch on YouTube →</a></em></sub>
</p>

## Try the in-browser flasher first

👉 **[tanksync.smartghar.org/firmware/](https://tanksync.smartghar.org/firmware/)**

No `esptool`, no Python, no CLI. Plug your board into USB, click Install, the browser does the flashing through WebSerial. Works on Chrome/Edge desktop. Takes ~45 seconds per board.

## Why TankSync — engineered to be reliable, not just smart

Most "smart tank" products treat the cloud as the product. TankSync treats **reliability** as the product — and the cloud as an optional layer of polish on top.

- **Works fully offline.** Hub keeps showing levels, lighting the LED ring, and beeping on overflow — even when your WiFi, your ISP, or our cloud is down. Local operation is the default; cloud is opt-in.
- **Long-range LoRa, no rooftop WiFi.** Sensor talks to the hub over 865 MHz LoRa — through concrete walls, between floors, across a property. Up to 5 km line-of-sight. The rooftop doesn't need WiFi. Ever.
- **Solar-powered transmitter.** Mounts on the tank lid. Charges in regular daylight, runs on a single 18650, deep-sleeps between readings. Months of autonomy. No wires to the tank.
- **Home Assistant native.** Auto-discovery via MQTT plus a dedicated [HACS integration](https://github.com/Techposts/smartghar-homeassistant) — every tank shows up as an HA device with live sensors, fill events, and editable settings.
- **Open at the core.** Firmware (AGPL-3.0), hardware (CC BY-SA 4.0), schematics, BOM, and flashing tools are all public. Self-host it. Fork it. Modify it. Audit it. No vendor lock-in.
- **Built for Indian realities.** Designed and tested through Delhi summer (45 °C ambient). UV-stabilised PETG, IP65 sealing, monsoon-ready. Engineered for terrace tanks, high-rise apartments, thick walls, and unreliable connectivity.

## Architecture

```
                    LoRa 865/915 MHz (up to 5 km, through walls)
                    ==============================================>
  TRANSMITTER                                          HUB (RECEIVER)
  ESP32-C3 SuperMini                                   ESP32 DevKit
  + JSN-SR04T Ultrasonic                               + RYLR998 LoRa
  + RYLR998 LoRa                                       + SH1106 OLED
  + 18650 + solar                                      + WS2812 LED ring
                                                       + WiFi (optional)
                                                          |
                                              +-----------+-----------+
                                              |                       |
                                       MQTT (TLS)              Local web UI
                                              |              192.168.x.x
                                    +---------+---------+
                                    |                   |
                              Home Assistant      Cloud dashboard
                              (HACS integration)  (optional, hosted)
```

## Hardware

| Component | Part | Approx cost (INR) |
|-----------|------|-------------------|
| Receiver MCU | ESP32 DevKit v1 | ₹300–400 |
| Transmitter MCU | ESP32-C3 SuperMini | ₹200 |
| LoRa module | REYAX RYLR998 (×2) | ₹650 each |
| Ultrasonic sensor | JSN-SR04T (waterproof) | ₹350 |
| Display | SH1106 1.3" OLED I²C | ₹250 |
| Battery | Protected 18650 + holder | ₹200 |
| Solar charger | CN3791 MPPT module | ₹120 |
| Boost converter | MT3608 3.7 V → 5 V | ₹50 |

Total: **~₹3,800-5,200 per complete system** (one hub + one tank). Per-tank addition: ~₹1,500.

📐 **[Detailed wiring + power chains →](hardware/wiring.md)**
📋 **[Full BOM →](hardware/BOM.csv)**

## Quick start

### Option 1: Browser flasher (easiest — no install)

👉 **[tanksync.smartghar.org/firmware/](https://tanksync.smartghar.org/firmware/)**

Plug your board into a USB port, pick the right card (Receiver Hub or Transmitter), click Install. Done in ~45 sec.

### Option 2: esptool.py (CLI)

Download the latest `.bin` from [Releases](../../releases).

```bash
# Receiver (ESP32 DevKit)
esptool.py --chip esp32 -b 460800 write_flash 0x10000 tanksync-receiver-rx-vX.Y.Z.bin

# Transmitter (ESP32-C3 SuperMini)
esptool.py --chip esp32c3 -b 460800 write_flash 0x10000 tanksync-transmitter-tx-vX.Y.Z.bin
```

### Option 3: Build from source

Prerequisites: [ESP-IDF v5.4+](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)

```bash
# Receiver Hub
cd firmware/Receiver-ESP32-DevKit
idf.py build
idf.py -p /dev/ttyUSB0 flash

# Transmitter
cd firmware/Transmitter-IDF
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash
```

### First boot

1. **Hub** starts in AP mode → connect to `TankSync-XXXX` WiFi from your phone
2. Captive portal opens (or visit `192.168.4.1`)
3. Configure home WiFi + (optional) MQTT broker + LoRa settings

## What you'll use it through

Two surfaces — pick either, or both. They show the same data.

<p align="center">
  <img src="docs/firmware/screenshots/pwa/dashboard-dark.png" width="38%" alt="TankSync PWA dashboard (phone)" />
  <img src="docs/firmware/screenshots/webui/tanks-home-glance.png" width="38%" alt="Hub local web UI (any device on the same Wi-Fi)" />
</p>
<p align="center">
  <sub><em>Left: the PWA at <a href="https://tanksync.smartghar.org">tanksync.smartghar.org</a> — works from anywhere. Right: the hub's local web UI — works fully offline. Full walkthrough in the <a href="https://github.com/Techposts/TankSync/wiki">Wiki</a>.</em></sub>
</p>

4. **Transmitter** pairs over the air — hold its `BOOT` button for 2 sec, hub LED turns green when paired

## Photos of a real build

<p align="center">
  <img src="hardware/photos/render-sensor-mount-iso.png" width="45%" alt="TankSync solar sensor — isometric render showing the BSP-threaded sensor boss + hex lock-nut" />
  <img src="hardware/photos/render-sensor-solar-top.png" width="45%" alt="TankSync solar sensor — top-down render showing the embedded solar panel" />
</p>
<p align="center">
  <sub><em>Renders of the production sensor — the threaded boss + hex lock-nut secure the sensor through a standard tank-lid hole; the solar panel sits flush on the case top.</em></sub>
</p>

<p align="center">
  <img src="hardware/photos/pcb-bare-top.jpg" width="30%" alt="Bare PCB, fresh from fab" />
  <img src="hardware/photos/pcb-populated-angle.jpg" width="30%" alt="Populated PCB, angled view" />
  <img src="hardware/photos/case-open-with-antenna.jpg" width="30%" alt="Opened case showing internals + antenna" />
</p>
<p align="center">
  <img src="hardware/photos/case-lid-with-solar.jpg" width="30%" alt="Lid with integrated solar panel pocket" />
  <img src="hardware/photos/case-sensor-mount.jpg" width="30%" alt="BSP-threaded sensor mount on a test tank lid" />
  <img src="hardware/photos/case-top-nut-thread.jpg" width="30%" alt="Close-up of the sensor-mount nut + thread" />
</p>

More photos + STL files for the case + schematics + 3D STEP models: **[hardware/](hardware/)**.

## Home Assistant integration

Two routes — pick whichever fits your setup:

1. **Native MQTT auto-discovery** — the hub publishes auto-discovery topics; tanks appear in HA as sensor entities with zero setup beyond pointing HA at the same broker. Read-only.
2. **HACS integration: SmartGhar** — full bidirectional control. Every tank is an HA device with grouped Sensors / Events / Configuration / Diagnostic entities, plus a hub device with buzzer + LED controls. **Capacity, sleep interval, samples-per-wake are editable from inside HA** and ride the same MQTT command channel as the PWA, so both stay in sync.

<p align="center">
  <img src="docs/firmware/screenshots/hacs/ha-hacs-listing.png"            width="18%" alt="HACS listing — SmartGhar v0.8.0" />
  <img src="docs/firmware/screenshots/hacs/ha-integration-overview.png"     width="18%" alt="Integration overview — 3 devices, 41 entities" />
  <img src="docs/firmware/screenshots/hacs/ha-device-page-top.png"          width="18%" alt="Tank device page — info + sensors" />
  <img src="docs/firmware/screenshots/hacs/ha-device-sensors-events.png"    width="18%" alt="Sensors + Events + Configuration" />
  <img src="docs/firmware/screenshots/hacs/ha-device-config-diagnostic.png" width="18%" alt="Configuration + Diagnostic — editable from HA" />
</p>

**HACS repo:** [github.com/Techposts/smartghar-homeassistant](https://github.com/Techposts/smartghar-homeassistant) · **Full setup + entity reference:** [HACS Integration wiki page](https://github.com/Techposts/TankSync/wiki/HACS-Integration)

## What's NOT in this repo (and why)

This is the open-source TankSync firmware + hardware mirror. The hosted cloud dashboard (PWA at [tanksync.smartghar.org](https://tanksync.smartghar.org)) is a separate **proprietary** product that adds:

- Remote access from anywhere (no port forwarding)
- Push notifications to your phone
- Multi-tank history + insights
- QR-code device linking
- Multi-hub fleet management for societies, farms, hotels

The firmware works fully **without** the cloud — local web UI on the hub gives you tank levels, settings, OTA updates, Home Assistant integration. Cloud is opt-in convenience, never a dependency.

## Developer Edition hardware

If you'd rather skip sourcing parts, flashing firmware, printing enclosures, and assembling hardware yourself, prebuilt TankSync Developer Edition kits are available for preorder.

Each kit includes:
- assembled RX hub
- assembled TX sensor node
- flashed firmware
- LoRa modules preconfigured
- waterproof enclosure set
- OLED display + local web UI
- access to the hosted TankSync PWA experience

The firmware and hardware files remain fully open for self-build deployments.

Preorders:
https://shop.smartghar.org

First batch is currently planned for end of July / early August 2026.

## Licenses

| Component | License | What this means |
|---|---|---|
| Firmware (`firmware/`) | [AGPL-3.0](LICENSE) | Free for personal + community use. Commercial users who modify and distribute must also open-source their changes under AGPL. |
| Hardware (`hardware/`) | [CC BY-SA 4.0](hardware/LICENSE) | Attribution + ShareAlike. Build it, sell it, modify it — credit the source and share-alike. |
| HA Integration | [MIT](https://github.com/Techposts/smartghar-homeassistant/blob/main/LICENSE) (separate repo) | Frictionless for HA ecosystem. |

**Why AGPL on firmware?** It keeps TankSync open for hobbyists and HA users while preventing commercial vendors from repackaging the firmware into a closed product. If you want a non-AGPL commercial license for embedded use, reach out to the maintainer.

## Contributing

Issues and PRs welcome. Read the [wiring guide](hardware/wiring.md) before opening hardware-related issues.

## Author + brand

**Ravi Singh** ([@ravis1ngh on YouTube](https://www.youtube.com/@ravis1ngh)) — solo-building open-source home infrastructure in India under the **TechPosts Media** / **SmartGhar** banner. Design, firmware, hardware, PCB layouts, and 3D-printed enclosures — all done in-house.

**TankSync** is part of the **SmartGhar** ecosystem ([smartghar.org](https://smartghar.org)) — calm, local-first smart-home infrastructure engineered for real-world Indian deployments.
