// glide-pico: Black Box Freedom II (Adder Free-Flow) multi-monitor adapter for macOS.
//
// The KVM computer port connects to the Pico (PIO-USB host). The Pico connects to the
// Mac (native USB device) as a keyboard, a relative mouse and an absolute pointer.
// No software or permissions are needed on the Mac.
//
// KVM side (through two hubs):
//   21D1:0001 Glide and Switch mouse: report 0x63 = X, Y (0..32767, relative to ONE
//             monitor), monitor index. Position only, no buttons.
//   BenQ mouse: buttons and wheel. X/Y are zero in glide mode.
//   Launch keyboard: NKRO report IDs 3, 4, 5.
//
// macOS maps an absolute pointer onto the display that holds the cursor. So the Pico
// sends absolute X/Y unchanged while the monitor index stays the same. When the index
// changes, it puts the cursor on the exit edge and sends a relative "push" across that
// edge, so macOS moves the cursor onto the next display. The Pico learns which index
// is past which edge from the crossings it sees.
//
// Wiring (USB plug to the KVM computer port):
//   D+   -> GP20 (pin 26)        D- -> GP21 (pin 27)
//   GND  -> GND  (pin 28)        VBUS -> VBUS (pin 40, 5 V from the Mac)
//
// LED (GP25):
//   short blink 1 Hz        firmware runs, nothing detected on the KVM side
//   blink 5 Hz              KVM detected on D+/D-, enumeration not complete
//   on                      KVM devices enumerated
//   on + fast flicker       position reports arrive
//
// CDC serial log (/dev/cu.usbmodem*): event lines from boot, replayed each time a
// terminal opens the port. A terminal that sets 1200 baud restarts the Pico into the
// UF2 boot loader.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "tusb.h"
#include "host/hcd.h"
#include "pio_usb.h"
#include "usb_descriptors.h"

#define PIO_USB_DP_PIN 20     // D- is always DP + 1
#define LED_PIN        25
#define PUSH           40     // relative counts that move the cursor across a display edge
#define GAP_US         2000   // minimum time between the reports of one display change
#define MAXIDX         16     // the Windows driver also uses index & 0x0F
#define EDGE_NEAR      3000   // a crossing must exit and enter within this distance of an edge

//--------------------------------------------------------------------+
// Event log
//--------------------------------------------------------------------+
static char evlog[16 * 1024];  // keeps the first 16 KiB
static volatile uint32_t evlen;
static uint32_t sent;

static void ev_append(const char *s, uint32_t n) {
  uint32_t irq = save_and_disable_interrupts();
  if (evlen + n <= sizeof evlog) {
    memcpy(&evlog[evlen], s, n);
    evlen += n;
  }
  restore_interrupts(irq);
}

static int vfmt(char *buf, size_t size, const char *f, va_list ap) {
  int n = vsnprintf(buf, size, f, ap);
  if (n < 0) return 0;
  return n >= (int)size ? (int)size - 1 : n;
}

static void ev(const char *f, ...) {
  char buf[200];
  va_list ap;
  va_start(ap, f);
  int n = vfmt(buf, sizeof buf, f, ap);
  va_end(ap);
  ev_append(buf, (uint32_t)n);
}

// TinyUSB debug output (CFG_TUSB_DEBUG_PRINTF). Can run in interrupt context.
int dbg_printf(const char *f, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, f);
  int n = vfmt(buf, sizeof buf, f, ap);
  va_end(ap);
  ev_append(buf, (uint32_t)n);
  return n;
}

#if CFG_TUD_CDC
// Live-only line (heartbeat). Only when the event log is fully sent.
static void out(const char *f, ...) {
  if (!tud_cdc_connected() || sent < evlen) return;
  char buf[200];
  va_list ap;
  va_start(ap, f);
  int n = vfmt(buf, sizeof buf, f, ap);
  va_end(ap);
  if (tud_cdc_write_available() >= (uint32_t)n) tud_cdc_write(buf, (uint32_t)n);
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
  (void)itf; (void)rts;
  if (dtr) sent = 0;
}

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *coding) {
  (void)itf;
  if (coding->bit_rate == 1200) reset_usb_boot(0, 0);
}

static void log_task(void) {
  if (!tud_cdc_connected()) return;
  uint32_t n = evlen - sent, room = tud_cdc_write_available();
  if (n > room) n = room;
  if (n) sent += tud_cdc_write(&evlog[sent], n);
}
#else
// Work build (GLIDE_NO_CDC): no serial port, so no log output and no 1200-baud reboot.
static void out(const char *f, ...) { (void)f; (void)sent; }
static void log_task(void) {}
#endif

static uint32_t ms(void) { return time_us_32() / 1000; }

//--------------------------------------------------------------------+
// Keyboard queue (no key report may be lost)
//--------------------------------------------------------------------+
typedef struct { uint8_t id, len, data[40]; } kbd_item_t;
static kbd_item_t kq[16];
static uint8_t kq_head, kq_tail;
static uint32_t kq_drops;

static void kq_push(uint8_t id, uint8_t const *data, uint16_t len) {
  uint8_t next = (uint8_t)((kq_tail + 1) % 16);
  if (next == kq_head || len > sizeof kq[0].data) { kq_drops++; return; }
  kq[kq_tail].id = id;
  kq[kq_tail].len = (uint8_t)len;
  memcpy(kq[kq_tail].data, data, len);
  kq_tail = next;
}

static void kbd_task(void) {
  if (!tud_mounted()) { kq_head = kq_tail; return; }
  if (kq_head == kq_tail || !tud_hid_n_ready(HID_KBD)) return;
  kbd_item_t *it = &kq[kq_head];
  if (tud_hid_n_report(HID_KBD, it->id, it->data, it->len)) kq_head = (uint8_t)((kq_head + 1) % 16);
}

//--------------------------------------------------------------------+
// Pointer queue: absolute and relative reports, sent strictly in order
//--------------------------------------------------------------------+
enum { P_ABS, P_REL };
typedef struct {
  uint8_t type;
  bool sealed;  // an unsealed ABS at the tail is replaced by a newer ABS
  bool gap;     // wait GAP_US after the previous report before this one
  uint8_t buttons;
  int16_t a, b;  // ABS: x, y. REL: dx, dy
  int8_t wheel, pan;
} pact_t;

#define PQ_SIZE 32
static pact_t pq[PQ_SIZE];
static uint8_t pq_head, pq_tail;
static bool p_inflight;
static uint32_t p_sent_us, p_done_us, pq_drops;
static uint8_t buttons_state;

static void pq_push(pact_t const *p) {
  // An unsealed ABS at the tail (not yet sent) is replaced by a newer ABS.
  if (p->type == P_ABS && !p->sealed && pq_head != pq_tail) {
    pact_t *last = &pq[(pq_tail + PQ_SIZE - 1) % PQ_SIZE];
    if (last->type == P_ABS && !last->sealed) {
      bool gap = last->gap;
      *last = *p;
      last->gap = last->gap || gap;
      return;
    }
  }
  uint8_t next = (uint8_t)((pq_tail + 1) % PQ_SIZE);
  if (next == pq_head) { pq_drops++; return; }
  pq[pq_tail] = *p;
  pq_tail = next;
}

static void pq_abs(uint16_t x, uint16_t y, bool sealed, bool gap) {
  pact_t p = {.type = P_ABS, .sealed = sealed, .gap = gap, .a = (int16_t)x, .b = (int16_t)y};
  pq_push(&p);
}

static void pq_rel(int16_t dx, int16_t dy, int8_t wheel, bool gap) {
  pact_t p = {.type = P_REL, .sealed = true, .gap = gap, .buttons = buttons_state,
              .a = dx, .b = dy, .wheel = wheel};
  pq_push(&p);
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
  (void)report; (void)len;
  if (instance != HID_KBD) { p_inflight = false; p_done_us = time_us_32(); }
}

void tud_hid_report_failed_cb(uint8_t instance, hid_report_type_t type, uint8_t const *report, uint16_t n) {
  (void)type; (void)report; (void)n;
  if (instance != HID_KBD) { p_inflight = false; p_done_us = time_us_32(); }
}

static void pointer_task(void) {
  if (!tud_mounted()) { pq_head = pq_tail; p_inflight = false; return; }
  uint32_t now = time_us_32();
  if (p_inflight) {
    if (now - p_sent_us < 50000) return;
    p_inflight = false;  // no completion in 50 ms: do not stop the queue
  }
  if (pq_head == pq_tail) return;
  pact_t *p = &pq[pq_head];
  if (p->gap && now - p_done_us < GAP_US) return;
  uint8_t inst = p->type == P_ABS ? HID_ABS : HID_MOUSE;
  if (!tud_hid_n_ready(inst)) return;

  bool ok;
  if (p->type == P_ABS) {
    uint8_t r[4] = {(uint8_t)p->a, (uint8_t)((uint16_t)p->a >> 8), (uint8_t)p->b, (uint8_t)((uint16_t)p->b >> 8)};
    ok = tud_hid_n_report(HID_ABS, 0, r, sizeof r);
  } else {
    uint8_t r[7] = {p->buttons, (uint8_t)p->a, (uint8_t)((uint16_t)p->a >> 8),
                    (uint8_t)p->b, (uint8_t)((uint16_t)p->b >> 8), (uint8_t)p->wheel, (uint8_t)p->pan};
    ok = tud_hid_n_report(HID_MOUSE, 0, r, sizeof r);
  }
  if (ok) {
    pq_head = (uint8_t)((pq_head + 1) % PQ_SIZE);
    p_inflight = true;
    p_sent_us = now;
  }
}

//--------------------------------------------------------------------+
// Display tracking (method 2)
//--------------------------------------------------------------------+
enum { E_L, E_R, E_T, E_B };  // opposite edge = e ^ 1
static const char edge_name[4] = {'L', 'R', 'T', 'B'};
static const int8_t dir_x[4] = {-1, 1, 0, 0}, dir_y[4] = {0, 0, -1, 1};

static int8_t adj[MAXIDX][4];  // adj[i][e] = index past edge e of index i, -1 = unknown
static int cur_disp = -1;      // index of the Mac display that holds the cursor (assumed)
static int last_idx = -1;
static uint16_t lx, ly;
static uint32_t glide_reports, last_glide_ms, crossings, reentries, moves, nopath;

// Nearest edge of a position, and its distance from that edge.
static int nearest_edge(uint16_t x, uint16_t y, uint16_t *dist) {
  uint16_t d[4] = {x, (uint16_t)(32767 - x), y, (uint16_t)(32767 - y)};
  int e = 0;
  for (int i = 1; i < 4; i++) if (d[i] < d[e]) e = i;
  *dist = d[e];
  return e;
}

// Record that index b is past edge e of index a. Remove older links between a and b
// on other edges, and older links that this edge replaces.
static void learn_edge(int a, int e, int b) {
  for (int k = 0; k < 4; k++) {
    if (adj[a][k] == b) adj[a][k] = -1;
    if (adj[b][k] == a) adj[b][k] = -1;
  }
  int old_b = adj[a][e], old_a = adj[b][e ^ 1];
  if (old_b >= 0 && adj[old_b][e ^ 1] == a) adj[old_b][e ^ 1] = -1;
  if (old_a >= 0 && adj[old_a][e] == b) adj[old_a][e] = -1;
  adj[a][e] = (int8_t)b;
  adj[b][e ^ 1] = (int8_t)a;
}

// Breadth-first search over the learned edges. Writes the edge sequence to edges[].
static int find_path(int from, int to, uint8_t *edges) {
  int8_t prev[MAXIDX], prev_e[MAXIDX];
  uint8_t queue[MAXIDX], qh = 0, qt = 0;
  memset(prev, -1, sizeof prev);
  prev[from] = (int8_t)from;
  queue[qt++] = (uint8_t)from;
  while (qh < qt) {
    int d = queue[qh++];
    if (d == to) break;
    for (int e = 0; e < 4; e++) {
      int n = adj[d][e];
      if (n >= 0 && prev[n] < 0) { prev[n] = (int8_t)d; prev_e[n] = (int8_t)e; queue[qt++] = (uint8_t)n; }
    }
  }
  if (prev[to] < 0) return -1;
  int len = 0;
  uint8_t rev[MAXIDX];
  for (int d = to; d != from; d = prev[d]) rev[len++] = (uint8_t)prev_e[d];
  for (int i = 0; i < len; i++) edges[i] = rev[len - 1 - i];
  return len;
}

static void glide_report(uint16_t x, uint16_t y, int idx) {
  uint32_t now = ms(), gap = now - last_glide_ms;
  glide_reports++;
  last_glide_ms = now;

  if (last_idx >= 0 && idx != last_idx) {
    // A real crossing: exit near an edge, entry near the opposite edge. A return from
    // another host enters on the same side as it left, fails this test, and must not
    // create an edge. No time limit: the user can stop at the edge before the crossing.
    uint16_t d_exit, d_entry;
    int e = nearest_edge(lx, ly, &d_exit);
    int en = nearest_edge(x, y, &d_entry);
    bool real = en == (e ^ 1) && d_exit <= EDGE_NEAR && d_entry <= EDGE_NEAR;
    if (real) {
      learn_edge(last_idx, e, idx);
      crossings++;
    } else {
      reentries++;
    }
    ev("%s %lu %d->%d edge=%c exit=(%u,%u) entry=(%u,%u) gap=%lu\r\n", real ? "XING" : "REENTRY",
       (unsigned long)now, last_idx, idx, edge_name[e], lx, ly, x, y, (unsigned long)gap);
  }
  if (cur_disp < 0) cur_disp = idx;

  bool moved = false;
  if (idx != cur_disp) {
    uint8_t edges[MAXIDX];
    int n = find_path(cur_disp, idx, edges);
    if (n > 0) {
      int d = cur_disp;
      for (int i = 0; i < n; i++) {
        int e = edges[i];
        // Put the cursor on the exit edge, then push across it.
        uint16_t px = 16384, py = 16384;
        if (d == last_idx) { px = lx; py = ly; }
        if (e == E_L) px = 0;
        if (e == E_R) px = 32767;
        if (e == E_T) py = 0;
        if (e == E_B) py = 32767;
        pq_abs(px, py, true, true);
        pq_rel((int16_t)(dir_x[e] * PUSH), (int16_t)(dir_y[e] * PUSH), 0, true);
        d = adj[d][e];
      }
      moves++;
      moved = true;
      ev("MOVE %lu %d->%d steps=%d\r\n", (unsigned long)ms(), cur_disp, idx, n);
    } else {
      nopath++;
      ev("NOPATH %lu %d->%d\r\n", (unsigned long)ms(), cur_disp, idx);
    }
    cur_disp = idx;
  }
  pq_abs(x, y, false, moved);
  last_idx = idx;
  lx = x;
  ly = y;
}

//--------------------------------------------------------------------+
// Host side (KVM)
//--------------------------------------------------------------------+
static uint8_t glide_addr, glide_inst, kbd_addr, kbd_inst, mouse_addr, mouse_inst;  // addr 0 = none

void tuh_mount_cb(uint8_t addr) {
  uint16_t vid, pid;
  tuh_vid_pid_get(addr, &vid, &pid);
  ev("MOUNT %lu addr=%u vid=%04x pid=%04x\r\n", (unsigned long)ms(), addr, vid, pid);
}

void tuh_umount_cb(uint8_t addr) { ev("UNMOUNT %lu addr=%u\r\n", (unsigned long)ms(), addr); }

void tuh_hid_mount_cb(uint8_t addr, uint8_t inst, uint8_t const *desc, uint16_t len) {
  uint16_t vid, pid;
  tuh_vid_pid_get(addr, &vid, &pid);
  uint8_t proto = tuh_hid_interface_protocol(addr, inst);
  const char *role = "ignored";
  if (vid == 0x21d1 && pid == 0x0001) {
    glide_addr = addr; glide_inst = inst; role = "glide";
  } else if (len == KBD_DESC_LEN && memcmp(desc, desc_hid_keyboard, len) == 0) {
    kbd_addr = addr; kbd_inst = inst; role = "keyboard";
  } else if (proto == HID_ITF_PROTOCOL_MOUSE) {
    mouse_addr = addr; mouse_inst = inst; role = "mouse";
  }
  ev("HID %lu addr=%u inst=%u vid=%04x pid=%04x proto=%u desc_len=%u role=%s\r\n", (unsigned long)ms(),
     addr, inst, vid, pid, proto, len, role);
  if (strcmp(role, "ignored") != 0) tuh_hid_receive_report(addr, inst);
}

void tuh_hid_umount_cb(uint8_t addr, uint8_t inst) {
  if (addr == glide_addr && inst == glide_inst) glide_addr = 0;
  if (addr == kbd_addr && inst == kbd_inst) kbd_addr = 0;
  if (addr == mouse_addr && inst == mouse_inst) mouse_addr = 0;
  ev("HID-UNMOUNT %lu addr=%u inst=%u\r\n", (unsigned long)ms(), addr, inst);
}

void tuh_hid_report_received_cb(uint8_t addr, uint8_t inst, uint8_t const *r, uint16_t len) {
  if (addr == glide_addr && inst == glide_inst) {
    if (len >= 6 && r[0] == 0x63)
      glide_report((uint16_t)(r[1] | r[2] << 8), (uint16_t)(r[3] | r[4] << 8), r[5] & (MAXIDX - 1));
  } else if (addr == kbd_addr && inst == kbd_inst) {
    if (len >= 2) kq_push(r[0], &r[1], (uint16_t)(len - 1));
  } else if (addr == mouse_addr && inst == mouse_inst) {
    // BenQ report protocol: buttons, X int16, Y int16, wheel int8. Boot format as fallback.
    int16_t dx, dy;
    int8_t wheel = 0;
    if (len >= 6) {
      dx = (int16_t)(r[1] | r[2] << 8);
      dy = (int16_t)(r[3] | r[4] << 8);
      wheel = (int8_t)r[5];
    } else if (len >= 3) {
      dx = (int8_t)r[1];
      dy = (int8_t)r[2];
      if (len >= 4) wheel = (int8_t)r[3];
    } else {
      dx = dy = 0;
    }
    buttons_state = r[0] & 0x1f;
    pq_rel(dx, dy, wheel, false);
  }
  tuh_hid_receive_report(addr, inst);
}

// Keyboard LEDs (Caps Lock) from the Mac to the keyboard.
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type, uint8_t const *buf,
                           uint16_t size) {
  static uint8_t out_buf[9];
  if (instance != HID_KBD || type != HID_REPORT_TYPE_OUTPUT || !kbd_addr || size == 0 || size > 8) return;
  uint16_t n = 0;
  if (report_id) out_buf[n++] = report_id;  // with report IDs, the data starts with the ID
  memcpy(&out_buf[n], buf, size);
  tuh_hid_set_report(kbd_addr, kbd_inst, report_id, HID_REPORT_TYPE_OUTPUT, out_buf, (uint16_t)(n + size));
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type, uint8_t *buf,
                               uint16_t reqlen) {
  (void)instance; (void)report_id; (void)type; (void)buf; (void)reqlen;
  return 0;
}

//--------------------------------------------------------------------+
static uint8_t mounted_count(void) {
  uint8_t n = 0;
  // Hub addresses follow the device addresses.
  for (uint8_t a = 1; a <= CFG_TUH_DEVICE_MAX + CFG_TUH_HUB; a++) n += tuh_mounted(a) ? 1 : 0;
  return n;
}

static void led_task(uint32_t now, bool port, uint8_t mounted) {
  bool on;
  if (mounted) on = (now - last_glide_ms < 100) ? ((now / 40) & 1) : true;
  else if (port) on = (now / 100) & 1;
  else on = (now % 1000) < 100;
  gpio_put(LED_PIN, on);
}

int main(void) {
  // PIO-USB needs a system clock that is a multiple of 12 MHz.
  set_sys_clock_khz(120000, true);
  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, GPIO_OUT);
  memset(adj, -1, sizeof adj);

  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
  pio_cfg.pin_dp = PIO_USB_DP_PIN;
  tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
  // Report protocol, not boot protocol: NKRO keyboard, 16-bit mouse, vendor bytes.
  tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);

  ev("BOOT glide-pico 0.4 dp=GP%u dm=GP%u push=%d\r\n", PIO_USB_DP_PIN, PIO_USB_DP_PIN + 1, PUSH);
  tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO};
  tusb_init(BOARD_TUD_RHPORT, &dev_init);
  tusb_rhport_init_t host_init = {.role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_AUTO};
  tusb_init(BOARD_TUH_RHPORT, &host_init);

  uint32_t next_hb = 0;
  bool last_port = false;
  while (true) {
    tud_task();
    tuh_task();
    kbd_task();
    pointer_task();
    log_task();

    uint32_t now = ms();
    bool port = hcd_port_connect_status(BOARD_TUH_RHPORT);
    if (port != last_port) {
      ev("PORT %lu %s\r\n", (unsigned long)now, port ? "connected" : "disconnected");
      last_port = port;
    }
    uint8_t mounted = mounted_count();
    led_task(now, port, mounted);
    if ((int32_t)(now - next_hb) >= 0) {
      next_hb = now + 2000;
      out("HB %lu port=%d mounted=%u mac=%d cur=%d glide=%lu xing=%lu reentry=%lu move=%lu nopath=%lu drops=%lu/%lu\r\n",
          (unsigned long)now, port, mounted, tud_mounted(), cur_disp, (unsigned long)glide_reports,
          (unsigned long)crossings, (unsigned long)reentries, (unsigned long)moves, (unsigned long)nopath, (unsigned long)kq_drops,
          (unsigned long)pq_drops);
    }
#if CFG_TUD_CDC
    tud_cdc_write_flush();
#endif
  }
}
