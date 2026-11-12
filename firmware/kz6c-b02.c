// KZ6C B02.1 prototype firmware, derived from KZ9C and PadKit.
// Six active-low keys: P11, P17, P16, P30, P14, P32.
// Seven RGB pixels on P34: D1-D3 SK6812MINI-E under the MX keys, then D4-D7 SK6812-EC20 at the
// case edge (left, bottom, right, top). T1-T3 flash D4/D5/D6 (their side) and D7 together.
// P31 controls the P-channel LED power switch, low enables it.
// Three profiles; T1+T3 (keys 4+6) changes profile after about 0.74 s.
// USB suspend and wake functionality require validation on actual hardware.
// Config protocol is inherited from KZ9C. See ATTRIBUTION.md for upstream notices.
// This binary is for the B02 design only and is not validated for an existing device.

#include <config.h>
#include <system.h>
#include <gpio.h>
#include <delay.h>
#include <neo.h>
#include <usb_conkbd.h>
#include <usb_hid.h>
#include <dataflash.h>

// The USB interrupt vector must live in the file with main() (SDCC), or USB never starts.
void USB_interrupt(void);
void USB_ISR(void) __interrupt(INT_NO_USB) {
  USB_interrupt();
}

#define NKEY            6                // keys
#define NPX             7                // 3 MX LEDs + 4 underglow LEDs
#define NRGB            (NPX*3)
#define FLAG_IDLE_DIM      0x02           // idle auto-dim enabled
#define FLAG_GAME          0x04           // "lights never get in the way": no strip writes while keys are busy

// ===================================================================================
// Persistent config (DataFlash, 128 bytes)
// ===================================================================================
#define CFG_MAGIC0         0x4B          // 'K'
#define CFG_MAGIC1         0x36          // 'Z'
#define CFG_VER            0xB1          // 0x97: + checksum byte; 0x96 images are taken over once
#define NPROF              3
#define CFG_VER_OLD        0xB1
#define CFG_SUM            126           // checksum of bytes 0..125, written last
#define CFG_LEN            127           // 3 + bright, profile, flags, idle(2) + 4 hues + 72 keymap + 27 touch + effect + 4 SOCD + 9 key colours + debounce
#define CFG_HUE            8
#define CFG_KEYMAP         12
#define CFG_TOUCH          84
#define CFG_EFFECT         111           // touch points: 12 bits each (u, v), 3 bytes per key
#define CFG_SOCD           112
#define CFG_KEYHUE         116
#define CFG_DEBOUNCE       125           // ms
#define TQ(x)              (((x) & 0x7FF8) | 4)   // touch point as stored: 12 bits, centre of the step
#define NEFFECT            5             // 0 static, 1 breathe, 2 rainbow, 3 reactive (glow on press), 4 off

uint8_t  g_brightness;                    // 0..255 master scale
uint8_t  g_effect;                        // NEFFECT effects, saved
__xdata uint8_t g_keyhue[NKEY];           // own colour of a key (all profiles), 0xFF = the profile's
__xdata uint8_t g_socd[NPROF];            // SOCD pair per profile: key a << 4 | key b, 0xFF = off
uint8_t  socd_sup = 0xFF;                 // key of the pair held but overridden by the later one
uint8_t  anim = 0;                        // animation phase (LED passes / 2)
uint8_t  g_flags;                         // FLAG_* bits
uint16_t g_idle_timeout;                  // idle-dim timeout, ms/100
__xdata uint8_t g_rgb[NRGB];              // 9 x RGB base colors (from the profile's hue)
__xdata uint16_t g_keymap[NPROF][NKEY];   // QMK keycodes per profile
__xdata uint8_t g_hue[NPROF];             // profile colours
uint8_t  g_prof;                          // active profile
uint16_t combo = 0;                       // loops K1 + K9 held
__xdata uint16_t g_touch[NKEY*2];         // 9 x {u, v} touch points, 0..32767
uint16_t g_idle_thresh;                   // derived: idle-dim threshold in loops

__xdata uint8_t dfbuf[CFG_LEN];           // DataFlash (de)serialize scratch
__xdata uint8_t vout[32];                 // vendor INPUT report scratch
__xdata uint8_t cmd[32];                  // command snapshot copied out of the ISR latch

// ===================================================================================
// LED engine state
// ===================================================================================
__xdata uint8_t flash[NPX];              // per-key press feedback, fades
__xdata uint8_t cur[NRGB];                // currently shown frame (fades toward tgt)
__xdata uint8_t tgt[NRGB];                // target frame
#define KMASK_ALL          0x003F
uint16_t kmask = KMASK_ALL;               // last key states, bit=1 released (9 keys)
// Clock: Timer0 high byte = 192 us ticks (16 MHz / 12 / 256), t0hi counts its overflows.
#define TICK_MS(ms)        ((uint16_t)((ms) * 26 / 5))   // ms -> 192 us ticks
uint8_t  t0hi = 0;
uint8_t  tick = 0;                        // LED passes
uint16_t now = 0;                         // current time in 192 us ticks (wraps every 12.6 s)
uint16_t led_t = 0, edge_t = 0;           // last LED pass, last key edge
uint8_t  g_deb;                           // debounce ms (saved)
uint16_t deb_t;                           // debounce in ticks
__xdata uint16_t kt[NKEY];                // per-key time of the last edge
uint16_t idle = 0;                        // Timer0 overflows (~49 ms) since last input
#define T0_MS              49            // Timer0, 16-bit at Fsys/12: overflows every 49.15 ms
#define COMBO_T0           15           // ~0.74 s in Timer0 overflows
uint8_t  dimmed = 0;                      // idle dim shown now
uint8_t  ledtest = 0xFF;                  // 0x50,0x20 LED test pixel (0xFF = off)
__xdata uint8_t ledtest_rgb[3];
#define KZ_TOUCH           0x7E00        // QMK keycode (QK_KB_0): this key touches the phone's screen
#define FW_MAJOR           2
#define FW_MINOR           0             // 1.7: sent[] per key, block until released, resend dropped reports, stale-command guard
__xdata uint16_t kcount[NKEY];            // presses per key (0x50,14), not saved
__xdata uint16_t kchat[NKEY];             // chatter: a press sooner than 15 ms after the release
__xdata uint16_t kup_t[NKEY];             // time of the last release (0 = none yet)

// Default profiles (QMK keycodes)
__code uint16_t DEFKEYS[NPROF][NKEY] = {
  {0x1D, 0x1B, 0x06, 0x29, 0x35, 0x2C},
  {KZ_TOUCH, KZ_TOUCH, KZ_TOUCH, KZ_TOUCH, KZ_TOUCH, KZ_TOUCH},
  {KZ_TOUCH, KZ_TOUCH, KZ_TOUCH, KZ_TOUCH, KZ_TOUCH, KZ_TOUCH}
};
__code uint8_t DEFHUE[NPROF] = {234, 120, 85};   // osu! pink, GD phone (as kz9c), GD platformer green
__code uint8_t DEFSOCD[NPROF] = {0xFF, 0xFF, 0x01};
__code uint8_t LEDMAP[NPX] = {0, 1, 2, 3, 4, 5, 6};
__code uint16_t DEFTOUCH[NKEY*2] = {
  8192,8192, 8192,16384, 8192,24575,
  24575,8192, 24575,16384, 24575,24575
};

// Touch screen: 3 report slots; t_key[s] = key on slot s (0xFF free). A lifted slot gets
// an id no key uses (0x70+s): two entries with one id in a report would lift a fresh touch.
#define NTS                3
uint8_t  t_down = 0;                       // bit s: slot s touching
__xdata uint8_t t_key[NTS] = {0xFF, 0xFF, 0xFF};
__xdata uint8_t t_id[NTS] = {0x70, 0x71, 0x72};
__xdata uint8_t treport[19];

// ===================================================================================
// Config helpers
// ===================================================================================
uint8_t hr, hg, hb;                        // hue_rgb() result
static void hue_rgb(uint8_t h) {                  // hue 0..255 -> full-saturation RGB
  uint8_t f = (uint8_t)((h % 43) * 6);
  switch(h / 43) {
    case 0:  hr = 255; hg = f; hb = 0; break;
    case 1:  hr = 255 - f; hg = 255; hb = 0; break;
    case 2:  hr = 0; hg = 255; hb = f; break;
    case 3:  hr = 0; hg = 255 - f; hb = 255; break;
    case 4:  hr = f; hg = 0; hb = 255; break;
    default: hr = 255; hg = 0; hb = 255 - f; break;
  }
}

static void cfg_recompute(void) {
  uint8_t i;
  uint32_t t = (uint32_t)g_idle_timeout * 100UL / T0_MS;   // ms/100 -> Timer0 overflows
  g_idle_thresh = (t > 0xFFFFUL) ? 0xFFFF : (uint16_t)t;
  deb_t = TICK_MS(g_deb);
  for(i = 0; i < NKEY; i++) {
    hue_rgb(g_keyhue[i] != 0xFF ? g_keyhue[i] : g_hue[g_prof]);
    g_rgb[i*3] = hr; g_rgb[i*3+1] = hg; g_rgb[i*3+2] = hb;
  }
  hue_rgb(g_hue[g_prof]);
  g_rgb[18] = hr; g_rgb[19] = hg; g_rgb[20] = hb;
}

static void cfg_defaults(void) {
  uint8_t i;
  uint8_t p;
  g_brightness   = 77;
  g_prof         = 0;
  g_effect       = 0;                             // static
  g_flags        = FLAG_IDLE_DIM;                 // idle-dim on, game priority off
  g_deb          = 5;
  g_idle_timeout = 3000;                          // 300000 ms ~= 5 min (matches v0.1)
  for(p = 0; p < NPROF; p++) {
    g_hue[p] = DEFHUE[p]; g_socd[p] = DEFSOCD[p];
    for(i = 0; i < NKEY; i++) g_keymap[p][i] = DEFKEYS[p][i];
  }
  for(i = 0; i < NKEY*2; i++) g_touch[i] = TQ(DEFTOUCH[i]);
  for(i = 0; i < NKEY; i++) g_keyhue[i] = 0xFF;
}

static void cfg_serialize(void) {
  uint8_t i;
  dfbuf[0] = CFG_MAGIC0; dfbuf[1] = CFG_MAGIC1; dfbuf[2] = CFG_VER;
  uint8_t p, j = CFG_KEYMAP;
  dfbuf[3] = g_brightness; dfbuf[4] = g_prof; dfbuf[5] = g_flags; dfbuf[CFG_EFFECT] = g_effect; dfbuf[CFG_DEBOUNCE] = g_deb;
  dfbuf[6] = (uint8_t)(g_idle_timeout & 0xFF);
  dfbuf[7] = (uint8_t)(g_idle_timeout >> 8);
  for(p = 0; p < NPROF; p++) {
    dfbuf[CFG_HUE + p] = g_hue[p]; dfbuf[CFG_SOCD + p] = g_socd[p];
    for(i = 0; i < NKEY; i++) {
      dfbuf[j++] = (uint8_t)(g_keymap[p][i] >> 8);
      dfbuf[j++] = (uint8_t)(g_keymap[p][i] & 0xFF);
    }
  }
  for(i = 0, j = CFG_TOUCH; i < NKEY; i++) {
    uint16_t u = g_touch[i*2] >> 3, v = g_touch[i*2+1] >> 3;
    dfbuf[j++] = (uint8_t)u;
    dfbuf[j++] = (uint8_t)((u >> 8) | (v << 4));
    dfbuf[j++] = (uint8_t)(v >> 4);
    dfbuf[CFG_KEYHUE + i] = g_keyhue[i];
  }
}

static void cfg_deserialize(void) {
  uint8_t i;
  uint8_t p, j = CFG_KEYMAP;
  g_brightness   = dfbuf[3];
  g_prof         = dfbuf[4] < NPROF ? dfbuf[4] : 0;
  g_effect       = dfbuf[CFG_EFFECT] < NEFFECT ? dfbuf[CFG_EFFECT] : 0;
  g_flags        = dfbuf[5];
  g_deb          = (dfbuf[CFG_DEBOUNCE] >= 1 && dfbuf[CFG_DEBOUNCE] <= 30) ? dfbuf[CFG_DEBOUNCE] : 5;
  g_idle_timeout = (uint16_t)dfbuf[6] | ((uint16_t)dfbuf[7] << 8);
  for(p = 0; p < NPROF; p++) {
    g_hue[p] = dfbuf[CFG_HUE + p]; g_socd[p] = dfbuf[CFG_SOCD + p];
    for(i = 0; i < NKEY; i++, j += 2) g_keymap[p][i] = ((uint16_t)dfbuf[j] << 8) | dfbuf[j + 1];
  }
  for(i = 0, j = CFG_TOUCH; i < NKEY; i++, j += 3) {
    g_touch[i*2]   = TQ((dfbuf[j] | ((uint16_t)(dfbuf[j+1] & 0x0F) << 8)) << 3);
    g_touch[i*2+1] = TQ(((dfbuf[j+1] >> 4) | ((uint16_t)dfbuf[j+2] << 4)) << 3);
    g_keyhue[i] = dfbuf[CFG_KEYHUE + i];
  }
}

// rotate-xor checksum of the image: a save cut off by unplugging leaves it wrong -> defaults
static uint8_t cfg_sum(void) {
  uint8_t i, s = 0x5A;
  for(i = 0; i < CFG_SUM; i++) s = (uint8_t)((s << 1) | (s >> 7)) ^ dfbuf[i];
  return s;
}

// USB reset generation the command being run was taken in: a reset after that drops its save
// and reply (the host that sent it is gone).
uint8_t cmd_gen = 0, in_cmd = 0;
#define CMD_STALE() (in_cmd && cmd_gen != HID_resetGen)
// A write that has started is finished even if a USB reset comes in the middle: stopping
// half-way would leave a mix of old and new bytes with a bad checksum (factory reset on the
// next start). A transactional write (second copy + switch) is impossible: the config is
// 127 of the 128 DataFlash bytes. So only a command that is already stale before the write
// is dropped; the window is the ~ms of the write itself.
static void cfg_save(void) {
  uint8_t i;
  if(CMD_STALE()) return;
  cfg_serialize();
  dfbuf[CFG_SUM] = cfg_sum();
  for(i = 0; i < CFG_LEN; i++)                   // only changed bytes: each write blocks USB IRQs
    if(DF_read(i) != dfbuf[i]) { DF_write(i, dfbuf[i]); WDT_reset(); }
}

static void cfg_load(void) {
  uint8_t i;
  for(i = 0; i < CFG_LEN; i++) dfbuf[i] = DF_read(i);
  if(dfbuf[0] == CFG_MAGIC0 && dfbuf[1] == CFG_MAGIC1 &&
     (dfbuf[2] == CFG_VER && dfbuf[CFG_SUM] == cfg_sum())) {
    i = dfbuf[2];
    cfg_deserialize();                            // valid image -> load it
  } else {
    cfg_defaults();                               // blank/bad flash -> factory + persist
    cfg_save();
  }
  cfg_recompute();
}

// ===================================================================================
// Touch screen (IF2, EP3): every report carries all 3 slots + the contact count
// ===================================================================================
static void touch_send(void) {
  uint8_t s, k, j = 0;
  for(s = 0; s < NTS; s++) {
    k = t_key[s]; if(k >= NKEY) k = 0;            // free slot: any point, not touching
    treport[j++] = ((t_down >> s) & 1) ? 3 : 0;   // tip switch + in range
    treport[j++] = t_id[s];
    treport[j++] = (uint8_t)(g_touch[k*2] & 0xFF);
    treport[j++] = (uint8_t)(g_touch[k*2] >> 8);
    treport[j++] = (uint8_t)(g_touch[k*2+1] & 0xFF);
    treport[j++] = (uint8_t)(g_touch[k*2+1] >> 8);
  }
  treport[18] = NTS;                              // contact count: all slots listed
  HID_sendTouch(treport, 19);
}
static void touch_key(uint8_t k, uint8_t down) {
  uint8_t s;
  if(k >= NKEY) return;
  if(down) {
    for(s = 0; s < NTS; s++) if(t_key[s] == 0xFF) break;
    if(s >= NTS) return;                          // 3 fingers already down: ignore
    t_key[s] = k; t_id[s] = k; t_down |= (uint8_t)(1 << s); touch_send();
  } else {
    for(s = 0; s < NTS; s++) if(t_key[s] == k) break;
    if(s >= NTS) return;                          // this key got no finger
    t_down &= (uint8_t)~(1 << s); touch_send();   // lift with its own id ...
    t_id[s] = 0x70 + s; t_key[s] = 0xFF;          // ... then park the slot
  }
}

// ===================================================================================
// Keystroke emission (respects the SUPPRESS flag; keymap-driven)
// ===================================================================================
// QMK keycode -> what the CH552 can send. Basic keys and modifiers go to the keyboard
// report, mod-wrapped keys (C(KC_C) ...) too, media keys to the consumer report, KZ_TOUCH to
// the touch screen. Mouse and other QMK features: nothing.
static uint16_t media(uint8_t c) {
  switch(c) {
    case 0xA8: return 0xE2;   // KC_MUTE
    case 0xA9: return 0xE9;   // KC_VOLU
    case 0xAA: return 0xEA;   // KC_VOLD
    case 0xAB: return 0xB5;   // KC_MNXT
    case 0xAC: return 0xB6;   // KC_MPRV
    case 0xAD: return 0xB7;   // KC_MSTP
    case 0xAE: return 0xCD;   // KC_MPLY
  }
  return 0;
}
// What each physical key actually pressed (0 = nothing): a release always lets go of exactly
// that, even if the key was remapped, the profile switched or SOCD changed meanwhile.
__xdata uint16_t sent[NKEY];
// Bit k: key k was let go of by a remap / profile switch / SOCD change while held. It stays
// silent (SOCD may not bring it back either) until it is physically released.
uint16_t kblk = 0;
static void act(uint8_t k, uint16_t kc, uint8_t down);
static void key_act(uint8_t k, uint8_t down) {
  uint16_t kc;
  if(down) {
    if(sent[k] || ((kblk >> k) & 1)) return;      // already pressed / blocked until released
    kc = g_keymap[g_prof][k];
    if(!kc) return;
    sent[k] = kc; act(k, kc, 1);
  } else {
    kc = sent[k];
    if(!kc) return;                               // this key pressed nothing
    sent[k] = 0; act(k, kc, 0);
  }
}
static void act(uint8_t k, uint16_t kc, uint8_t down) {
  uint8_t mod = 0, code, hi = (uint8_t)(kc >> 8);
  uint16_t con;
  if(kc == KZ_TOUCH) { touch_key(k, down); return; }
  if(hi >= 0x20) return;                          // other QMK features: not here
  code = (uint8_t)(kc & 0xFF);
  if(hi) { mod = hi & 0x0F; if(hi & 0x10) mod <<= 4; }
  if(code >= 0xE0 && code <= 0xE7) { mod |= (uint8_t)(1 << (code - 0xE0)); code = 0; }
  else if((con = media(code)) != 0) {
    if(!down) CON_release(con);
    else if(!CON_press(con)) sent[k] = 0;         // all 4 media slots busy: not sent, not held
    return;
  }
  else if(code < 0x04 || code > 0xA4) code = 0;
  if(!mod && !code) return;
  if(down) KBD_pressRaw(mod, code); else KBD_releaseRaw(mod, code);
}

// ===================================================================================
// Command processing (main-loop context, out of the ISR)
// ===================================================================================
// A held key gets a new meaning: let go of the old one, the new one starts on the next press.
static void key_block(uint8_t k) {
  if((kmask >> k) & 1) return;                    // not held
  key_act(k, 0);
  kblk |= (uint16_t)1 << k;
  if(socd_sup == k) socd_sup = 0xFF;
}

// Switch profile: let go of every held key of the old one first, then save and flash all LEDs.
static void set_profile(uint8_t p) {
  uint8_t k;
  for(k = 0; k < NKEY; k++) key_block(k);
  socd_sup = 0xFF;
  g_prof = p;
  cfg_recompute(); cfg_save();
  for(k = 0; k < NPX; k++) flash[k] = 255;
}

static void reply(void) { if(!CMD_STALE()) HID_sendVendor(vout, cmd_gen); }   // re-checked after the EP2 wait

static void process_cmd(void) {
  uint8_t c = cmd[0], i, k;
  for(i = 0; i < 32; i++) vout[i] = cmd[i];       // replies echo the command
  switch(c) {
    case 0x01: vout[1] = 0x00; vout[2] = 0x0C; break;            // VIA protocol 12
    case 0x04:                                                    // get key [layer, row, col]
    case 0x05:                                                    // set key [.., hi, lo]
      k = cmd[2] * 3 + cmd[3]; i = cmd[1];
      if(i >= NPROF || cmd[2] >= 2 || cmd[3] >= 3) { vout[4] = 0; vout[5] = 0; break; }
      if(c == 0x05) {
        if(i == g_prof) key_block(k);                             // never leave it held
        g_keymap[i][k] = ((uint16_t)cmd[4] << 8) | cmd[5];
        cfg_save();
      }
      vout[4] = (uint8_t)(g_keymap[i][k] >> 8); vout[5] = (uint8_t)(g_keymap[i][k] & 0xFF);
      break;
    case 0x11: vout[1] = NPROF; break;                            // layers = profiles
    case 0x0C: vout[1] = 0; break;                                // no macros
    case 0x0D: vout[1] = 0; vout[2] = 0; break;                   // macro buffer 0 bytes
    case 0xFE:                                                    // Vial
      for(i = 0; i < 32; i++) vout[i] = 0;
      if(cmd[1] == 0x05) vout[0] = 1;                             // unlocked
      else if(cmd[1] == 0x0D && cmd[2] == 0) vout[0] = 0;         // 0 tap dances
      else vout[0] = 0xFF;
      break;
    case 0x50:
      i = cmd[2];
      switch(cmd[1]) {
        case 1: if(i < NPROF && i != g_prof) set_profile(i);    /* fall through */
        case 0:
          vout[2] = g_prof; vout[3] = NPROF;
          vout[4] = (uint8_t)(~kmask & 0xFF);                     // keys held now, 9 bits LE
          vout[5] = 0;
          vout[10] = FW_MAJOR; vout[11] = FW_MINOR;
          break;
        case 2: if(CMD_STALE()) break; WDT_stop(); BOOT_now(); break;  // stale: host gone; WDT would reset the bootloader
        case 4:
          if(i < NKEY) {
            g_touch[i*2]   = (uint16_t)cmd[3] | ((uint16_t)cmd[4] << 8);
            g_touch[i*2+1] = (uint16_t)cmd[5] | ((uint16_t)cmd[6] << 8);
            g_touch[i*2] = TQ(g_touch[i*2]); g_touch[i*2+1] = TQ(g_touch[i*2+1]);
            cfg_save();
          }
          /* fall through */
        case 3:
          if(i < NKEY) {
            vout[6] = (uint8_t)(g_touch[i*2] & 0xFF);   vout[7] = (uint8_t)(g_touch[i*2] >> 8);
            vout[8] = (uint8_t)(g_touch[i*2+1] & 0xFF); vout[9] = (uint8_t)(g_touch[i*2+1] >> 8);
          }
          break;
        case 14:                                                  // press counters, 6 keys from i
          for(k = 0; k < 24; k++) vout[3 + k] = 0;
          for(k = 0; k < 6; k++) if(i + k < NKEY) {
            vout[3 + k*4] = (uint8_t)(kcount[i + k] & 0xFF);
            vout[4 + k*4] = (uint8_t)(kcount[i + k] >> 8);
            vout[5 + k*4] = (uint8_t)(kchat[i + k] & 0xFF);
            vout[6 + k*4] = (uint8_t)(kchat[i + k] >> 8);
          }
          vout[27] = NKEY;
          break;
        case 15: for(k = 0; k < NKEY; k++) { kcount[k] = 0; kchat[k] = 0; } break;
        case 0x20:                                                // LED test: raw pixel i
          ledtest = i; ledtest_rgb[0] = cmd[3]; ledtest_rgb[1] = cmd[4]; ledtest_rgb[2] = cmd[5];
          break;
        case 13:                                                  // profile colour, saved
          if(i < NPROF) { g_hue[i] = (cmd[3] & 2) ? cmd[4] : DEFHUE[i]; cfg_recompute(); cfg_save(); }
          /* fall through */
        case 12:
          for(k = 3; k < 32; k++) vout[k] = 0;
          if(i < NPROF) { vout[3] = g_hue[i] != DEFHUE[i] ? 2 : 0; vout[4] = g_hue[i]; }
          break;
        case 0x23:                                                // lights: [0x23, save, effect, brightness]
          if(i) {
            if(cmd[3] < NEFFECT) g_effect = cmd[3];
            g_brightness = cmd[4];
            cfg_save();
          }
          vout[3] = g_effect; vout[4] = g_brightness; vout[5] = NEFFECT;
          break;
        case 0x26:                                                // key colour: [0x26, k, set, hue (0xFF = profile's)]
          if(i < NKEY) {
            if(cmd[3]) { g_keyhue[i] = cmd[4]; cfg_recompute(); cfg_save(); }
            vout[4] = g_keyhue[i];
          }
          break;
        case 0x27:                                                // game: [0x27, save, debounce ms, game priority]
          if(i) {
            if(cmd[3] >= 1 && cmd[3] <= 30) g_deb = cmd[3];
            if(cmd[4]) g_flags |= FLAG_GAME; else g_flags &= (uint8_t)~FLAG_GAME;
            cfg_recompute(); cfg_save();
          }
          vout[3] = g_deb; vout[4] = (g_flags & FLAG_GAME) ? 1 : 0;
          break;
        case 0x25:                                                // SOCD pair: [0x25, p, set, pair]
          if(i < NPROF) {
            if(cmd[3]) {
              if(i == g_prof && socd_sup != 0xFF) { key_act(socd_sup, 1); socd_sup = 0xFF; }
              g_socd[i] = cmd[4]; cfg_save();
              if(i == g_prof) {                                   // both keys of the new pair held already:
                uint8_t s = g_socd[i], pa = s >> 4, pb = s & 0x0F;  // keep one, let go of the other
                if(s != 0xFF && pa < NKEY && pb < NKEY && pa != pb &&
                   !((kmask >> pa) & 1) && !((kmask >> pb) & 1)) { key_act(pa, 0); socd_sup = pa; }
              }
            }
            vout[4] = g_socd[i];
          }
          break;
      }
      break;
    default: vout[0] = 0xFF; break;                               // VIA: unhandled
  }
  reply();
}

// ===================================================================================
// LED rendering
// ===================================================================================
// Move the shown frame 1/4 of the way toward the target each pass for smooth fades.
static uint8_t fade_step(void) {
  uint8_t i, changed = 0;
  for(i = 0; i < NRGB; i++) {
    uint8_t c = cur[i], t = tgt[i];
    if(c == t) continue;
    changed = 1;
    if(c < t) { uint8_t d = (t - c) >> 2; cur[i] = c + (d ? d : 1); }
    else      { uint8_t d = (c - t) >> 2; cur[i] = c - (d ? d : 1); }
  }
  return changed;
}

// Output the shown frame: effect wave, master brightness, idle dim, then the
// press-feedback (white) and IDENTIFY (white) overlays at the last stage.
extern volatile uint8_t rgb_powered;
__xdata uint8_t frame[NRGB];               // output in chain order
#define LED_BUDGET         4200          // estimate: up to ~330 mA at 20 mA/channel
static void render_out(void) {
  uint8_t i, j = 0, wave = 255, eff = g_effect;
  if(eff == 1) {                                    // breathe: triangle wave, ~2.6 s
    wave = (anim & 0x80) ? (uint8_t)((255 - anim) << 1) : (uint8_t)(anim << 1);
    if(wave < 30) wave = 30;
  }
  for(i = 0; i < NPX; i++, j += 3) {
    uint16_t r = cur[j], g = cur[j+1], b = cur[j+2];
    uint8_t f = flash[i], p = LEDMAP[i] * 3;
    if(eff == 2) { hue_rgb((uint8_t)(anim + i * 28)); r = hr; g = hg; b = hb; }   // rainbow
    else if(eff >= 3) { r = 0; g = 0; b = 0; }      // reactive / off: dark until pressed
    r = (r * wave) >> 8; g = (g * wave) >> 8; b = (b * wave) >> 8;
    if(eff == 3 && f) { r = (uint16_t)cur[j] * f >> 8; g = (uint16_t)cur[j+1] * f >> 8; b = (uint16_t)cur[j+2] * f >> 8; }
    r = (r * g_brightness) >> 8; g = (g * g_brightness) >> 8; b = (b * g_brightness) >> 8;
    if(dimmed) { r >>= 2; g >>= 2; b >>= 2; }
    if(f && eff < 3) {                              // press feedback (white) overlay
      f = (uint8_t)((uint16_t)f * g_brightness >> 8);
      if(f > r) r = f; if(f > g) g = f; if(f > b) b = f;
    }
    frame[p] = (uint8_t)r; frame[p+1] = (uint8_t)g; frame[p+2] = (uint8_t)b;
  }
  {                                                 // power cap: a WS2812 channel at 255 draws ~20 mA;
    uint16_t sum = 0;                               // estimated channel current under ~330 mA; verify on hardware
    for(i = 0; i < NRGB; i++) sum += frame[i];
    if(sum > LED_BUDGET) {                          // one division per frame (slow on the 8051)
      uint8_t k = (uint8_t)(((uint32_t)LED_BUDGET << 8) / sum);
      for(i = 0; i < NRGB; i++) frame[i] = (uint8_t)(((uint16_t)frame[i] * k) >> 8);
    }
  }
  if(ledtest != 0xFF) {                             // LED test: only raw pixel ledtest
    for(i = 0; i < NRGB; i++) frame[i] = 0;
    if(ledtest < NPX) {
      j = ledtest * 3;
      frame[j] = ledtest_rgb[0]; frame[j+1] = ledtest_rgb[1]; frame[j+2] = ledtest_rgb[2];
    }
  }
  // SK6812MINI-E and SK6812-EC20 (12 mA): <=70% grayscale when all three colours are lit.
  for(i = 0; i < NRGB; i += 3) {
    uint8_t m = frame[i];
    if(frame[i+1] > m) m = frame[i+1];
    if(frame[i+2] > m) m = frame[i+2];
    if(frame[i] && frame[i+1] && frame[i+2] && m > 178) {
      frame[i] = (uint8_t)((uint16_t)frame[i] * 178 / m);
      frame[i+1] = (uint8_t)((uint16_t)frame[i+1] * 178 / m);
      frame[i+2] = (uint8_t)((uint16_t)frame[i+2] * 178 / m);
    }
  }
  EA = 0;
  // Atomically avoid driving an unpowered chain after suspend or USB reset.
  if(!rgb_powered || !UsbConfig || (USB_MIS_ST & bUMS_SUSPEND)) { EA = 1; return; }
  for(i = 0; i < NRGB; i += 3) NEO_writeColor(frame[i], frame[i+1], frame[i+2]);
  EA = 1;
  NEO_latch();
}

// SOCD (last input wins) for the profile's key pair: pressing one key of the pair while the
// other is held lets go of the other; releasing the newer one presses the held one again.
static uint8_t socd_other(uint8_t k) {
  uint8_t s = g_socd[g_prof], a = s >> 4, b = s & 0x0F;
  if(s == 0xFF || a >= NKEY || b >= NKEY || a == b) return 0xFF;
  return k == a ? b : (k == b ? a : 0xFF);
}
static void key_down(uint8_t k) {
  uint8_t o = socd_other(k);
  if(o != 0xFF && !((kmask >> o) & 1)) {            // the other one is held
    if(socd_sup != o) key_act(o, 0);
    socd_sup = o;
  }
  key_act(k, 1);
}
static void key_up(uint8_t k) {
  uint8_t o = socd_other(k);
  kblk &= (uint16_t)~((uint16_t)1 << k);            // physically up: the block ends here only
  if(socd_sup == k) { socd_sup = 0xFF; return; }    // already let go
  key_act(k, 0);
  if(o != 0xFF && socd_sup == o) { socd_sup = 0xFF; key_act(o, 1); }
}

#define TOUCH() do { idle = 0; } while(0)

// One key, eager: an edge goes out at once, then the key ignores its contact for g_deb ms.
#define KEY_STEP(PORTPIN, IDX) do { \
    if((uint16_t)(now - kt[IDX]) >= deb_t) { \
      uint8_t nw = PIN_read(PORTPIN) ? 1 : 0; \
      if(nw != ((kmask >> (IDX)) & 1)) { \
        kt[IDX] = now; edge_t = now; \
        if(!nw) { key_down(IDX); flash[IDX] = 255; if((IDX) >= 3) flash[6] = 255; kcount[IDX]++; \
                  if(kup_t[IDX] && (uint16_t)(now - kup_t[IDX]) < TICK_MS(15)) kchat[IDX]++; } \
        else    { key_up(IDX); kup_t[IDX] = now | 1; } \
        kmask = nw ? (kmask | (1U << (IDX))) : (kmask & (uint16_t)~(1U << (IDX))); \
        TOUCH(); \
      } \
    } \
  } while(0)

// ===================================================================================
// Main
// ===================================================================================
volatile uint8_t rgb_powered = 0;
volatile uint8_t rgb_starting = 0;

void main(void) {
  uint8_t i;

  CLK_config();
  PIN_high(P31); PIN_output(P31);              // P-MOS gate high = LED supply off
  PIN_input_PU(P11); PIN_input_PU(P17); PIN_input_PU(P16);
  PIN_input_PU(P30); PIN_input_PU(P14); PIN_input_PU(P32);
  DLY_ms(10);
  if(!PIN_read(P11)) BOOT_now();                    // K1 held at plug-in -> bootloader
  NEO_init();                                  // data low before enabling LED supply
  KBD_clearHeld();                                  // before any key: counters live in uncleared xdata
  for(i = 0; i < NKEY; i++) {                       // same for the per-key state (xdata is not cleared)
    flash[i] = 0; kt[i] = 0; kup_t[i] = 0; kcount[i] = 0; kchat[i] = 0; sent[i] = 0;
  }
  for(i = 0; i < NRGB; i++) tgt[i] = 0;
  ledtest_rgb[0] = ledtest_rgb[1] = ledtest_rgb[2] = 0;
  for(i = 0; i < NTS; i++) { t_key[i] = 0xFF; t_id[i] = 0x70 + i; }   // also set by initialisers,
  for(i = 0; i < sizeof(KBD_report); i++) KBD_report[i] = 0;          // but do not rely on them
  for(i = 0; i < sizeof(CON_report); i++) CON_report[i] = 0;
  KBD_report[0] = 1; CON_report[0] = 2;             // report IDs
  t_down = 0; kblk = 0; socd_sup = 0xFF; HID_lost = 0; UsbConfig = 0;
  KBD_init();
  cfg_load();                                       // DataFlash or factory defaults
  for(i = 0; i < NRGB; i++) cur[i] = 0;             // fade up from black
  for(i = 0; i < NPX; i++) flash[i] = 0;
  TMOD = (TMOD & 0xF0) | 0x01; TH0 = 0; TL0 = 0; TF0 = 0; TR0 = 1;   // Timer0: free-running clock
  DLY_ms(50);
  WDT_start();                                      // ~1 s period: only after slow setup

  while(1) {
    if(!UsbConfig || (USB_MIS_ST & bUMS_SUSPEND)) {
      if(rgb_powered) {
        PIN_low(P34); PIN_high(P31); rgb_powered = 0;
      }
    } else if(!rgb_powered) {
      // Suspend interrupt can invalidate startup while the LED rail rises.
      EA = 0;
      if(UsbConfig && !(USB_MIS_ST & bUMS_SUSPEND)) {
        PIN_low(P34); rgb_starting = 1; PIN_low(P31);
      }
      EA = 1;
      // B02 gate network: R6 10k + C12 4.7uF gate-source, RC ~43 ms. The P-MOSFET
      // starts conducting after ~15 ms and is fully on by ~100 ms; measure the ramp.
      DLY_ms(120);
      EA = 0;
      if(rgb_starting && UsbConfig && !(USB_MIS_ST & bUMS_SUSPEND)) {
        rgb_powered = 1;
      } else {
        PIN_low(P34); PIN_high(P31); rgb_powered = 0;
      }
      rgb_starting = 0;
      EA = 1;
      if(rgb_powered) render_out();
    }
    // --- vendor commands from IF1 (parsed out of the ISR) ---
    if(HID_cmdPending) {
      EA = 0;
      for(i = 0; i < 32; i++) cmd[i] = HID_cmdBuf[i];
      HID_cmdDone();                                // re-arm EP2 OUT (it NAKs while one is pending)
      cmd_gen = HID_resetGen;
      EA = 1;
      in_cmd = 1;                                   // a USB reset from here on: no save, no reply
      if(!CMD_STALE()) process_cmd();
      in_cmd = 0;
    }

    // --- a report the host did not take within ~400 ms (sleep, busy host) or forgot on a
    //     USB bus reset: send the current state again once configured and the endpoint is free ---
    if(HID_lost && UsbConfig) {
      if((HID_lost & HID_LOST_KBD) && !HID_EP1_writeBusyFlag) HID_sendReport(KBD_report, sizeof(KBD_report));
      else if((HID_lost & HID_LOST_CON) && !HID_EP1_writeBusyFlag) HID_sendReport(CON_report, sizeof(CON_report));
      if((HID_lost & HID_LOST_TOUCH) && !HID_EP3_writeBusyFlag) touch_send();
    }

    // --- clock; idle dim, T1 + T3 held ~0.74 s = next profile ---
    if(TF0) {
      TF0 = 0; t0hi++;
      if(idle < 0xFFFF) idle++;
      if((kmask & 0x0028) == 0) {
        if(combo < 0xFFFF) combo++;
        if(combo == COMBO_T0) set_profile((uint8_t)((g_prof + 1) % NPROF));
      } else combo = 0;
    }
    now = ((uint16_t)t0hi << 8) | TH0;

    // --- keys: scanned every pass (no delay), per-key debounce in real time ---
    KEY_STEP(P11, 0); KEY_STEP(P17, 1); KEY_STEP(P16, 2);
    KEY_STEP(P30, 3); KEY_STEP(P14, 4); KEY_STEP(P32, 5);

    // --- LED engine every 5 ms; the strip is written only when the picture changes (a write
    //     stops interrupts ~0.3 ms), plus a refresh every ~1.3 s. Game priority: no writes while
    //     a key is held or within 10 ms of an edge ---
    if((uint16_t)(now - led_t) >= TICK_MS(5)) {
      uint8_t busy = 0, dim = (g_flags & FLAG_IDLE_DIM) && idle >= g_idle_thresh;
      led_t = now; tick++;
      if(tick & 1) anim++;
      for(i = 0; i < NRGB; i++) tgt[i] = g_rgb[i];
      for(i = 0; i < NPX; i++)
        if(flash[i]) { flash[i] = (flash[i] > 8) ? flash[i] - 8 : 0; busy = 1; }   // ~160 ms glow
      if(fade_step()) busy = 1;
      if((g_flags & FLAG_GAME) && (kmask != KMASK_ALL || (uint16_t)(now - edge_t) < TICK_MS(10)) && ledtest == 0xFF) busy = 0;
      else if(rgb_powered && (busy || dim != dimmed || g_effect || ledtest != 0xFF || tick == 0)) { dimmed = dim; render_out(); }
    }

    WDT_reset();
  }
}
