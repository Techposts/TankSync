# Circular v1 Enclosure — Current Production

Production case as of **May 2026**. Designed for the circular custom PCB documented in [`../../pcb/`](../../pcb/). Tested through Delhi summer (45 °C ambient) — PETG holds shape, integrated solar pocket doesn't delaminate, EPDM gasket compresses reliably for IP65 sealing.

---

## STL files

| File | What it is | Print orientation |
|---|---|---|
| [`base-holder.stl`](base-holder.stl) | Bottom shell — holds the PCB + 18650 holder | Flat side down, no supports inside the cavity |
| [`top-with-solar.stl`](top-with-solar.stl) | Top lid with integrated solar-panel pocket | Solar pocket facing up, **tree supports** on the underside of the lip |
| [`sensor-screw.stl`](sensor-screw.stl) | BSP-threaded boss the JSN-SR04T screws through | Threads up; supports on the gland-side flange |
| [`nut.stl`](nut.stl) | Locking nut that mates with the sensor-screw boss from inside the tank lid | Flat down, no supports |

Print all four for a complete unit.

---

## Bill of additional parts

In addition to the PCB BOM ([`../../BOM.csv`](../../BOM.csv)):

| Item | Spec | Where it goes |
|---|---|---|
| Solar panel | 6 V, 1–2 W, 60–80 mm matching the lid pocket | Sits in the top-with-solar pocket; wires through small grommet |
| EPDM gasket | 1–2 mm × 60 mm OD circular, NBR also OK | Between base and lid |
| M3 stainless screws | 4×, 12 mm length | Lid → base, through gasket |
| M3 stainless threaded inserts (optional) | Heat-set, 4× | Base — for stronger repeated open/close |
| Cable gland | M12 PA66 IP65, 4–6 mm cable range | Bottom of base — sensor cable exit |
| BSP thread (sensor mount) | Matches `sensor-screw.stl` threads | Field-drilled into the tank lid |

---

## Assembly sequence (TLDR)

1. Heat-set M3 inserts into the four corners of `base-holder.stl` (if using).
2. Drop the populated TX PCB into the base; align mount holes; tape the 18650 holder in place.
3. Feed the sensor cable through the cable gland from outside, leaving slack inside. Screw-terminal both ends to the PCB.
4. Mount the panel into the lid pocket; thread its wires through the small grommet; solder to the PCB's panel input.
5. Place the EPDM gasket on the base rim. Screw the lid down with the four M3 stainless screws — moderate torque (~1 Nm). The gasket compresses to ~50 % thickness.
6. The `sensor-screw.stl` + `nut.stl` mount on the **tank lid itself**, with the JSN-SR04T head facing down into the tank. Apply Teflon tape on the BSP threads for water seal.

A photo-by-photo build guide is in the wiki: [github.com/Techposts/TankSync/wiki/Hardware-Build](https://github.com/Techposts/TankSync/wiki/Hardware-Build).

---

## Design notes

- The lid solar pocket is **slightly oversize** to allow a 1 mm silicone bead around the panel for UV sealing. Don't pour epoxy — it traps heat against the panel.
- The sensor-screw boss is intentionally tall (~20 mm) so the JSN-SR04T transducer head sits clear of the tank lid's internal surface. Lid-touching sensors give phantom echoes from the lid material.
- The PETG print is rated for outdoor use, but **direct UV will yellow it over years**. If you care about aesthetics long-term, add a 1 mm clear-coat or use ASA.
- IP65 was the design target — tested with light rain + hose spray. Not tested under submersion (and the tank shouldn't ever submerge the TX anyway).
