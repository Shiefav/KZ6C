// ===================================================================================
// USB HID Functions for CH551, CH552 and CH554
// ===================================================================================
//
// /* PadKit local patch vs wagiminator upstream (CH552 stack)
//  * ---------------------------------------------------------------------------
//  * v0.2 replaces the old single-interface LED-report parsing with a transport
//  * for the composite device's vendor interface (IF1, EP2 IN+OUT):
//  *
//  *   - HID_sendReport()  : keyboard IN on EP1 (unchanged; keeps the watchdog-fed
//  *                         busy-wait bail-out patch from v0.1).
//  *   - HID_sendVendor()  : 32-byte vendor INPUT report on EP2 IN (INPUT_EVENT /
//  *                         CONFIG_DUMP / FW_INFO / ACK). Same watchdog-fed wait.
//  *   - HID_EP2_OUT()     : latches the 32-byte vendor OUTPUT command into
//  *                         HID_cmdBuf + sets HID_cmdPending; the main loop
//  *                         (padkit.c) parses it out of interrupt context so the
//  *                         DataFlash writes and reply sends never run in the ISR.
//  *
//  * Upstream declares only HID_init() and HID_sendReport(). See usb_hid.c.
//  * ---------------------------------------------------------------------------
//  */

#pragma once
#include <stdint.h>

void HID_init(void);                                      // setup USB-HID
void HID_sendReport(__xdata uint8_t* buf, uint8_t len);   // keyboard IN (EP1)
void HID_sendVendor(__xdata uint8_t* buf, uint8_t gen);   // vendor IN, 32 bytes (EP2); dropped if USB reset gen != gen
void HID_sendTouch(__xdata uint8_t* buf, uint8_t len);    // touch screen IN (EP3)

// Vendor OUTPUT command latch (host -> device, EP2 OUT). The ISR copies the raw
// 32-byte report here and raises HID_cmdPending; the main loop consumes it.
extern volatile uint8_t HID_cmdPending;                   // 1 = new command waiting
extern __xdata uint8_t  HID_cmdBuf[32];                   // latest 32-byte command
extern volatile uint8_t HID_resetGen;                    // counts USB resets
extern volatile uint8_t HID_lost;                        // reports dropped after a timeout:
#define HID_LOST_KBD   1                                 //   keyboard (EP1, id 1)
#define HID_LOST_CON   2                                 //   media (EP1, id 2)
#define HID_LOST_TOUCH 4                                 //   touch (EP3)
#define HID_LOST_ALL   (HID_LOST_KBD | HID_LOST_CON | HID_LOST_TOUCH)
extern volatile uint8_t UsbConfig;                       // 0 = not configured (after a bus reset)
extern volatile __bit HID_EP1_writeBusyFlag;
extern volatile __bit HID_EP3_writeBusyFlag;
void HID_epReset(uint8_t ep);                             // ISR: SET_CONFIGURATION (0xFF) / CLEAR_FEATURE on an IN ep
void HID_ep2OutReset(void);                               // ISR: CLEAR_FEATURE on EP2 OUT
void HID_cmdDone(void);                                  // command taken: ACK the next OUT
