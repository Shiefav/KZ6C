# KZ6C — six-button CH552 USB macropad (prototype B02.1)

A 64 × 64 mm two-layer PCB with three MX-style switches and three 6 × 6 mm tactile switches, USB-C, CH552G and seven individually addressable RGB LEDs. This is an experimental design: it has not yet been validated on a physical prototype and is not a production-ready release.

- `hardware/`: editable KiCad 10 schematic, PCB, project and project-local libraries.
- `firmware/`: CH552 firmware source, vendored dependencies, attribution and firmware license. Requires SDCC; run `make all` inside that directory to build. No flashing is performed by this command.
- `case/`: preliminary sandwich-case OpenSCAD, DXF outlines and an approximate assembly generator. Switch dimensions, fasteners, LED illumination and actual USB clearance require physical fit checks.

## Prototype checks

Check the tactile switch footprint, pad dimensions, height and paired contacts against actual purchased parts. Check component orientation including the SK6812-EC20 LEDs, and the BOM/CPL in the assembly provider's viewer. Measure the current in full brightness and USB suspend, inrush when the LEDs switch on, USB startup and wake. Test key combinations, flashing recovery and fit of switches, case and USB cable on real hardware.

The development firmware currently reports FEED:4B16 as a temporary test identity. This is not a permanent assigned identifier; obtain an appropriate VID/PID before product release. A successful KiCad DRC/ERC does not prove electrical, USB or mechanical compliance.

## Attribution and licensing

The firmware is based on PadKit and the CH552 USB/NeoPixel stack by Stefan Wagner. Attribution, modifications and the CC BY-SA 3.0 firmware license are in `firmware/ATTRIBUTION.md` and `firmware/LICENSE`. Original hardware and case are CC BY-SA 4.0. The adapted KiCad USB-C footprint carries the KiCad library exception. See the root LICENSE and hardware/ATTRIBUTION.md. Firmware is CC BY-SA 3.0; retain its upstream notices.
