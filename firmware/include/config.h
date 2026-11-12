// ===================================================================================
// PadKit — user configuration (CH552G 6-key + rotary-knob macropad)
// ===================================================================================
// Editable build-time config for the vendored CH552 stack. The NeoPixel pin, the
// USB VID/PID and the descriptor strings are read from here; padkit.c drives the
// full 6-key + encoder pin set directly (see its PIN_input_PU calls), so only
// PIN_NEO below is consumed by the runtime.
//
// VID/PID: KeypadZone FEED:4B19 (the app knows our models by PID).


#pragma once

// Pin definitions
#define PIN_NEO             P34         // WS2812 chain, one LED under each key
// Keys: P1.1 P1.7 P1.6 / P3.0 P1.4 P3.2; LED power control: P3.1

// NeoPixel configuration
#define NEO_GRB                         // type of pixel: NEO_GRB or NEO_RGB

// USB device descriptor
#define USB_VENDOR_ID       0xFEED      // KeypadZone (as the RP2040 pads)
#define USB_PRODUCT_ID      0x4B16      // KZ6C prototype identity; allocation not verified
#define USB_DEVICE_VERSION  0x0200      // B02 prototype

// USB configuration descriptor
#define USB_MAX_POWER_mA    400         // requested maximum; must be verified on hardware

// USB descriptor strings
#define MANUFACTURER_STR    'K','e','y','p','a','d','Z','o','n','e'
#define PRODUCT_STR         'K','e','y','p','a','d','Z','o','n','e',' ','K','Z','6','C'
#define SERIAL_STR          'C','H','5','5','2','x','H','I','D'
#define INTERFACE_STR       'K','e','y','p','a','d','Z','o','n','e',' ','K','Z','6','C',' ','K','e','y','b','o','a','r','d'
#define INTERFACE_STR2      'K','e','y','p','a','d','Z','o','n','e',' ','K','Z','6','C',' ','C','o','n','f','i','g'
