// ===================================================================================
// USB HID Functions for CH551, CH552 and CH554
// ===================================================================================
//
// /* PadKit local patch vs wagiminator upstream (CH552 stack)
//  * ---------------------------------------------------------------------------
//  * This file carries PadKit's functional patches to the upstream HID transport.
//  * The matching declarations live in usb_hid.h (also patched).
//  *
//  * PATCH 1 — HID_sendReport(): watchdog-fed busy-wait with a hard bail-out.
//  *   Upstream spins on HID_EP1_writeBusyFlag with a bare `while(...);` loop.
//  *   With the hardware watchdog enabled and a burst of input events, any pause
//  *   in the host's EP1 polling let that loop run past the watchdog period and
//  *   reset the chip mid-use (USB re-enumeration = multi-second outage). The fix
//  *   feeds the watchdog inside the wait, paces it (~50us), and gives up after a
//  *   bounded time (~400ms) by dropping the report instead of hanging forever if
//  *   the host has genuinely stopped listening (suspend/unplug).
//  *
//  * PATCH 2 — composite EP2 vendor transport (NEW in v0.2).
//  *   HID_setup()/HID_reset() now bring EP2 up bidirectionally (IN + OUT) for the
//  *   vendor interface. HID_sendVendor() ships a 32-byte INPUT report from the
//  *   +64 transmit half of the EP2 buffer (same watchdog-fed pacing as PATCH 1).
//  *   HID_EP2_OUT() latches the 32-byte host command for the main loop instead of
//  *   the old inline LED parsing, so DataFlash writes / reply sends stay out of
//  *   the ISR. HID_EP2_IN() clears the vendor TX busy flag.
//  * ---------------------------------------------------------------------------
//  */

#include "ch554.h"
#include "usb.h"
#include "usb_hid.h"
#include "usb_descr.h"
#include "usb_handler.h"
#include "system.h"
#include "delay.h"

// ===================================================================================
// Variables and Defines
// ===================================================================================

volatile __bit HID_EP1_writeBusyFlag = 0;                  // EP1 (keyboard IN) busy
volatile __bit HID_EP2_writeBusyFlag = 0;                  // EP2 (vendor IN) busy
volatile __bit HID_EP3_writeBusyFlag = 0;                  // EP3 (touch IN) busy

volatile uint8_t HID_cmdPending = 0;                       // main-loop command flag
volatile uint8_t HID_resetGen;                             // +1 on every USB reset (kz9c: drop stale commands)
volatile uint8_t HID_lost;                                 // HID_LOST_* reports dropped after a timeout
__xdata uint8_t  HID_cmdBuf[32];                           // latest vendor command

// ===================================================================================
// Front End Functions
// ===================================================================================

// Setup USB HID
void HID_init(void) {
  USB_init();
  UEP1_T_LEN  = 0;
  UEP2_T_LEN  = 0;
  UEP3_T_LEN  = 0;
}

// Send HID keyboard report (EP1 IN). See PATCH 1.
// kz9c: not configured (after a bus reset, before SET_CONFIGURATION) -> no wait, mark the
// report lost; the main loop sends the current state once the host has configured us.
void HID_sendReport(__xdata uint8_t* buf, uint8_t len) {
  uint8_t i;
  uint16_t guard = 0;
  uint8_t bit = (buf[0] == 2) ? HID_LOST_CON : HID_LOST_KBD;
  while(HID_EP1_writeBusyFlag && UsbConfig) {
    WDT_reset();
    DLY_us(50);
    if(++guard > 8000) break;                              // ~400ms: give up, resend later
  }
  if(HID_EP1_writeBusyFlag || !UsbConfig) { HID_lost |= bit; return; }
  HID_lost &= (uint8_t)~bit;                               // the latest state goes out now
  for(i=0; i<len; i++) EP1_buffer[i] = buf[i];             // copy report to EP1 buffer
  UEP1_T_LEN = len;                                        // set length to upload
  HID_EP1_writeBusyFlag = 1;                               // set busy flag
  UEP1_CTRL = UEP1_CTRL & ~MASK_UEP_T_RES | UEP_T_RES_ACK; // upload data and respond ACK
}

// Send a 32-byte vendor INPUT report (EP2 IN). The transmit data lives in the
// upper half (offset EP2_IN_OFFSET) of EP2's bidirectional buffer. Called from
// the main loop (never the ISR). Same watchdog-fed wait + bail-out as PATCH 1.
// kz9c: gen = HID_resetGen when the command was taken. A bus reset during the wait (or
// before it) means a new USB session: the old reply is dropped, never sent to the new host.
void HID_sendVendor(__xdata uint8_t* buf, uint8_t gen) {
  uint8_t i;
  uint16_t guard = 0;
  while(HID_EP2_writeBusyFlag) {
    WDT_reset();
    DLY_us(50);
    if(++guard > 8000) return;                              // ~400ms: give up
  }
  if(gen != HID_resetGen || !UsbConfig) return;            // reset since the command: stale
  for(i=0; i<32; i++) EP2_buffer[EP2_IN_OFFSET + i] = buf[i];
  UEP2_T_LEN = 32;
  HID_EP2_writeBusyFlag = 1;
  UEP2_CTRL = UEP2_CTRL & ~MASK_UEP_T_RES | UEP_T_RES_ACK;  // upload, respond ACK (TX bits only)
}

// Send a touch screen report (EP3 IN), KeypadZone. Same watchdog-fed wait.
void HID_sendTouch(__xdata uint8_t* buf, uint8_t len) {
  uint8_t i;
  uint16_t guard = 0;
  while(HID_EP3_writeBusyFlag && UsbConfig) {
    WDT_reset();
    DLY_us(50);
    if(++guard > 8000) break;                              // ~400ms: give up, resend later
  }
  if(HID_EP3_writeBusyFlag || !UsbConfig) { HID_lost |= HID_LOST_TOUCH; return; }
  HID_lost &= (uint8_t)~HID_LOST_TOUCH;
  for(i=0; i<len; i++) EP3_buffer[i] = buf[i];
  UEP3_T_LEN = len;
  HID_EP3_writeBusyFlag = 1;
  UEP3_CTRL = UEP3_CTRL & ~MASK_UEP_T_RES | UEP_T_RES_ACK;
}

// ===================================================================================
// HID-Specific USB Handler Functions
// ===================================================================================

// Setup HID endpoints: EP1 IN (keyboard), EP2 IN+OUT (vendor).
void HID_setup(void) {
  UEP1_DMA    = EP1_ADDR;                   // EP1 data transfer address
  UEP2_DMA    = EP2_ADDR;                   // EP2 data transfer address (OUT low / IN high)
  UEP3_DMA    = EP3_ADDR;                   // EP3 data transfer address (touch IN)
  UEP3_CTRL   = bUEP_AUTO_TOG | UEP_T_RES_NAK;
  UEP1_CTRL   = bUEP_AUTO_TOG               // EP1 auto flip
              | UEP_T_RES_NAK;              // EP1 IN returns NAK until armed
  UEP2_CTRL   = bUEP_AUTO_TOG               // EP2 auto flip
              | UEP_T_RES_NAK               // EP2 IN returns NAK until armed
              | UEP_R_RES_ACK;              // EP2 OUT accepts (ACK)
  UEP4_1_MOD  = bUEP1_TX_EN;                // EP1 TX enable
  UEP2_3_MOD  = bUEP2_RX_EN | bUEP2_TX_EN   // EP2 RX + TX enable (bidirectional)
              | bUEP3_TX_EN;                // EP3 TX enable (touch)
}

// Reset HID parameters
void HID_reset(void) {
  UEP1_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK;
  UEP2_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK | UEP_R_RES_ACK;
  UEP3_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK;
  HID_EP3_writeBusyFlag = 0;
  HID_EP1_writeBusyFlag = 0;
  HID_EP2_writeBusyFlag = 0;
  HID_cmdPending = 0;
  HID_resetGen++;
  // kz9c: the host forgets every report on a bus reset. Not configured until SET_CONFIGURATION;
  // the main loop (not this ISR) then sends the current keyboard/media/touch state again.
  UsbConfig = 0;
  HID_lost = HID_LOST_ALL;
}

// kz9c: SET_CONFIGURATION (ep = 0xFF: EP1/EP3 IN, EP2 IN+OUT) or CLEAR_FEATURE(ENDPOINT_HALT)
// on one endpoint (0x81/0x82/0x83 IN, 0x02 OUT) (ISR). The host resets its data toggle to DATA0; a report armed before is
// cancelled here (NAK), so its busy flag must be dropped too: otherwise it would stay set with
// nothing to send, and every later report would wait 400 ms and be lost. The main loop sends
// the current state again (HID_lost).
void HID_epReset(uint8_t ep) {
  if(ep == 0x81 || ep == 0xFF) {
    UEP1_T_LEN = 0;
    UEP1_CTRL = UEP1_CTRL & ~(bUEP_T_TOG | MASK_UEP_T_RES) | UEP_T_RES_NAK;
    HID_EP1_writeBusyFlag = 0;
    HID_lost |= HID_LOST_KBD | HID_LOST_CON;
  }
  if(ep == 0x82) {                                         // vendor reply: dropped, not resent
    UEP2_T_LEN = 0;
    UEP2_CTRL = UEP2_CTRL & ~(bUEP_T_TOG | MASK_UEP_T_RES) | UEP_T_RES_NAK;
    HID_EP2_writeBusyFlag = 0;
  }
  if(ep == 0xFF) {
    // SET_CONFIGURATION: both EP2 directions back to DATA0. A command latched or being run
    // belongs to the old configuration: drop it (HID_resetGen++ makes CMD_STALE skip its
    // save and reply) and accept the next OUT.
    UEP2_T_LEN = 0;
    UEP2_CTRL = UEP2_CTRL & ~(bUEP_T_TOG | bUEP_R_TOG | MASK_UEP_T_RES | MASK_UEP_R_RES)
              | UEP_T_RES_NAK | UEP_R_RES_ACK;
    HID_EP2_writeBusyFlag = 0;
    HID_cmdPending = 0;
    HID_resetGen++;
  }
  if(ep == 0x83 || ep == 0xFF) {
    UEP3_T_LEN = 0;
    UEP3_CTRL = UEP3_CTRL & ~(bUEP_T_TOG | MASK_UEP_T_RES) | UEP_T_RES_NAK;
    HID_EP3_writeBusyFlag = 0;
    HID_lost |= HID_LOST_TOUCH;
  }
}

// EP2 OUT halt cleared: DATA0; ACK only if no command waits for the main loop
// (otherwise HID_cmdDone() re-arms it, as after any command).
void HID_ep2OutReset(void) {
  UEP2_CTRL = UEP2_CTRL & ~(bUEP_R_TOG | MASK_UEP_R_RES) | (HID_cmdPending ? UEP_R_RES_NAK : UEP_R_RES_ACK);
}

// Endpoint 1 IN handler (keyboard report sent to host)
void HID_EP1_IN(void) {
  UEP1_T_LEN = 0;                                           // no data to send anymore
  UEP1_CTRL = UEP1_CTRL & ~MASK_UEP_T_RES | UEP_T_RES_NAK;  // default NAK
  HID_EP1_writeBusyFlag = 0;                                // clear busy flag
}

// Endpoint 2 IN handler (vendor report sent to host)
void HID_EP2_IN(void) {
  UEP2_T_LEN = 0;
  UEP2_CTRL = UEP2_CTRL & ~MASK_UEP_T_RES | UEP_T_RES_NAK;  // re-arm NAK (TX bits only)
  HID_EP2_writeBusyFlag = 0;
}

// Endpoint 3 IN handler (touch report sent to host)
void HID_EP3_IN(void) {
  UEP3_T_LEN = 0;
  UEP3_CTRL = UEP3_CTRL & ~MASK_UEP_T_RES | UEP_T_RES_NAK;
  HID_EP3_writeBusyFlag = 0;
}

// Endpoint 2 OUT handler (vendor command received from host). Latch the raw
// 32-byte report for the main loop; do no protocol work in the ISR.
void HID_EP2_OUT(void) {
  uint8_t len = USB_RX_LEN;
  uint8_t i;
  if(len > 32) len = 32;
  for(i = 0; i < len; i++) HID_cmdBuf[i] = EP2_buffer[i];   // OUT data in low half
  for(; i < 32; i++)       HID_cmdBuf[i] = 0;               // zero-pad short writes
  HID_cmdPending = 1;
  // kz9c: NAK further commands until the main loop has taken this one (HID_cmdDone),
  // so a quick second command waits in the host instead of overwriting the first
  UEP2_CTRL = UEP2_CTRL & ~MASK_UEP_R_RES | UEP_R_RES_NAK;
}

// Main loop: command copied out of HID_cmdBuf -> accept the next one
void HID_cmdDone(void) {
  HID_cmdPending = 0;
  UEP2_CTRL = UEP2_CTRL & ~MASK_UEP_R_RES | UEP_R_RES_ACK;
}
