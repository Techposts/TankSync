# TankSync Enclosures

Two case generations live here. **Print only the current revision unless you're explicitly building a legacy unit.**

| Folder | Status | PCB shape | Material | When to use |
|---|---|---|---|---|
| [`circular-v1/`](circular-v1/) | **Current — production** | Circular custom PCB | PETG | Always, for any new build |
| [`rectangular-v10/`](rectangular-v10/) | Legacy reference | Rectangular off-the-shelf carrier | PETG | Only if you already have v1.0 PCBs in inventory |

---

## Why two designs?

The original **rectangular v1.0** case was built around stacked off-the-shelf modules on a rectangular carrier — fine for the prototype, but the form factor wasn't ideal for the tank-lid mount, and the assembly involved many internal wires.

The **circular v1** is a ground-up redesign: a circular custom PCB sized to drop straight into the lid, with the ultrasonic sensor screw-mounted through a BSP-threaded boss, and an integrated solar-panel pocket on the top half. It was **tested through Delhi summer (45 °C ambient)** for thermal stability — the PETG holds shape, the panel doesn't delaminate, and the gasket compresses reliably.

The rectangular STL is kept here purely as reference for anyone who already fabbed those PCBs.

---

## Printing notes (shared across both designs)

- **Material**: PETG. *Not* PLA — sun + 45 °C summer = sag. *Not* ABS unless you've got an enclosure printer with good ventilation. ASA also works.
- **Layer height**: 0.2 mm structural / 0.16 mm cosmetic
- **Infill**: 30 % gyroid (good stiffness-to-weight + survives compression on screw-down)
- **Walls**: 4 perimeters minimum — strength matters more than print speed for outdoor parts
- **Supports**: needed on the sensor screw and (for circular-v1) the lid-with-solar piece. See per-design README for tree-vs-grid hint.
- **Brim**: 5 mm — PETG warp is real on first layer if the bed is below 80 °C
- **Bed**: 80–90 °C; nozzle 240–250 °C
- **Outdoor seal**: every case is designed to clamp a 1–2 mm EPDM gasket between lid and body. The screw torque does the sealing — don't skip the gasket if the tank is outdoors.

See `../../docs/hardware/printing-guide.md` (work in progress) for tested slicer profiles.

---

## Choosing between PETG colours

Black PETG runs hotter under sun (measured ~5 °C above ambient). The TX has thermal margin — 45 °C ambient + 5 °C tint + 5 °C internal dissipation = within the 18650's 65 °C derating. But if you live somewhere genuinely hot (Rajasthan summer, north Australia), **print white or light-grey** for the lid. The base can stay any colour you like.
