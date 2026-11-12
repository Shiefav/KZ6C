# Firmware attribution and changes

KZ6C firmware is adapted from PadKit firmware by the PadKit contributors,
including Dennis W: https://github.com/denniswjpg/padkit, based on the upstream
snapshot represented by commit cc10519. The original PadKit attribution is
preserved in `../LICENSES/PADKIT-ATTRIBUTION-ORIGINAL.md`.

PadKit attributes its CH552 USB and NeoPixel software stack to Stefan Wagner
(wagiminator), https://github.com/wagiminator. The precise Wagner upstream
snapshot used by PadKit was not recorded. Attribution to other upstream
contributors listed in the original PadKit notice is retained in that notice
and the original source-file headers. The `include/ch554.h` header carries a
WCH copyright notice; that notice remains in the file.

KeypadZone modified the PadKit firmware for the CH552G KZ6C six-key board:
GPIO mapping, USB HID descriptors and controls, profiles, touch reports,
addressable RGB output and power management. Compared with the PadKit snapshot,
`kz6c-b02.c` and files `include/usb_conkbd.c`, `usb_conkbd.h`, `usb_hid.c`,
`usb_hid.h`, `usb_descr.c`, `usb_descr.h`, `usb_handler.c`, `usb_handler.h`
and `config.h` contain KZ6C changes. The included source files retain their
existing copyright and license notices. Adapted firmware remains CC BY-SA 3.0;
see LICENSE and the complete license text in `../LICENSES/CC-BY-SA-3.0.txt`.

Firmware has been compiled with SDCC 4.6.0; the board remains untested on
physical hardware. No claim is made that the temporary USB identifiers are
allocated to this project.
