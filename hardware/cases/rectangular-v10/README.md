# Rectangular v1.0 Enclosure — Legacy

Original prototype case used during TankSync v1.0. **Kept here for reference only.** New builds should use [`../circular-v1/`](../circular-v1/) — that's the current production design.

You only want this folder if:

- You already fabbed rectangular v1.0 PCBs and have inventory to use up, or
- You're studying the design evolution for your own project.

---

## STL files

| File | What it is |
|---|---|
| [`main-body.stl`](main-body.stl) | Bottom shell of the original rectangular enclosure |
| [`top-lid.stl`](top-lid.stl) | Top lid (no solar pocket — this design had an external panel) |
| [`tpu-gasket.stl`](tpu-gasket.stl) | Printable TPU gasket (alternative to a die-cut EPDM gasket) |
| [`sensor-screw.stl`](sensor-screw.stl) | Sensor mount — matches the v1.0 sensor-cable layout |
| [`bolt-screw.stl`](bolt-screw.stl) | Captive screw used to clamp lid to body in the v1.0 design |

---

## Why it's legacy

The rectangular v1.0 case worked, but had drawbacks the circular v1 redesign solved:

1. **Form factor** — rectangular footprint doesn't match how most tank lids are shaped (round threaded openings), so mounting required adapters.
2. **Solar panel** — external, wired in through a grommet. Two more failure points (UV-aging cable + grommet seal).
3. **Assembly time** — more internal wiring (stacked modules on a rectangular carrier, vs the circular custom PCB which integrates everything).
4. **Print time** — larger XY footprint means longer prints.

The TPU printable gasket worked fine but most builders switched to die-cut EPDM (more consistent compression, cheaper at >10 units).

For any new build use [`../circular-v1/`](../circular-v1/).
