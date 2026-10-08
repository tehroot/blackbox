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
//
// Flash log (both builds): the same event lines go to the Pico's own flash, one slot
// for each boot (flashlog.h). Read them with tools/pico-log.sh in BOOTSEL mode. The
// learned edges are also in this log ("EDGES" lines), and the next boot restores them.
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
#include "flashlog.h"

#define PIO_USB_DP_PIN 20     // D- is always DP + 1
#define LED_PIN        25
#define PUSH           40     // relative counts that move the cursor across a display edge
#define GAP_US         2000   // minimum time between the reports of one display change
#define MAXIDX         16     // the Windows driver also uses index & 0x0F
#define EDGE_NEAR      3000   // a crossing must exit and enter within this distance of an edge
#define FW_VERSION     "0.5.1"
#define STAT_MS        600000 // a STAT line in the log every 10 minutes of use

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
  flog_append(s, n);
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

// Milliseconds since boot. Not time_us_32() / 1000: that wraps after 71.6 minutes.
static uint32_t ms(void) { return to_ms_since_boot(get_absolute_time()); }

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
static uint32_t glide_reports, last_glide_ms, crossings, reentries, moves, nopath, homes, guesses;

// Nearest edge of a position, and its distance from that edge.
static int nearest_edge(uint16_t x, uint16_t y, uint16_t *dist) {
  uint16_t d[4] = {x, (uint16_t)(32767 - x), y, (uint16_t)(32767 - y)};
  int e = 0;
  for (int i = 1; i < 4; i++) if (d[i] < d[e]) e = i;
  *dist = d[e];
  return e;
}

// Edge of a crossing: the exit is near edge e and the entry is near the opposite edge.
// All four edges are tested, not only the nearest one: at a corner, the nearest edge of
// the exit can be the side edge (exit (0,399), entry (0,32486) is a crossing of the top
// edge). Returns -1 if no edge agrees.
static int crossing_edge(uint16_t ex, uint16_t ey, uint16_t nx, uint16_t ny) {
  uint16_t d_exit[4] = {ex, (uint16_t)(32767 - ex), ey, (uint16_t)(32767 - ey)};
  uint16_t d_entry[4] = {nx, (uint16_t)(32767 - nx), ny, (uint16_t)(32767 - ny)};
  int best = -1;
  uint16_t best_d = EDGE_NEAR + 1;
  for (int e = 0; e < 4; e++) {
    uint16_t d = d_exit[e] > d_entry[e ^ 1] ? d_exit[e] : d_entry[e ^ 1];
    if (d < best_d) { best_d = d; best = e; }
  }
  return best;
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

// Write the learned edges to the log, for example "EDGES 0T1 1B0" (index 1 is past the
// top edge of index 0). The next boot restores the last EDGES line of the flash log.
static void log_edges(void) {
  char buf[MAXIDX * 4 * 7 + 16];
  int n = snprintf(buf, sizeof buf, "EDGES");
  for (int a = 0; a < MAXIDX; a++)
    for (int e = 0; e < 4; e++)
      if (adj[a][e] >= 0) n += snprintf(&buf[n], sizeof buf - (size_t)n, " %d%c%d", a, edge_name[e], adj[a][e]);
  ev("%s\r\n", buf);
}

static int parse_num(const char **p, const char *end) {
  int v = -1;
  while (*p < end && **p >= '0' && **p <= '9') { v = (v < 0 ? 0 : v * 10) + (**p - '0'); (*p)++; }
  return v;
}

// Restore the edges from the last EDGES line of the previous boot's log. Returns the
// number of links, or -1 if there is no valid line.
static int restore_edges(const char *p, uint32_t len) {
  const char *line = NULL, *end = p + len;
  for (uint32_t i = 0; i + 6 <= len; i++)
    if ((i == 0 || p[i - 1] == '\n') && memcmp(&p[i], "EDGES", 5) == 0 && (p[i + 5] == ' ' || p[i + 5] == '\r'))
      line = &p[i + 5];
  if (!line) return -1;
  int8_t t[MAXIDX][4];
  memset(t, -1, sizeof t);
  int links = 0;
  while (line < end && *line != '\r' && *line != '\n') {
    if (*line == ' ') { line++; continue; }
    int a = parse_num(&line, end);
    const char *ep = line < end ? memchr(edge_name, *line, 4) : NULL;
    if (ep) line++;
    int b = parse_num(&line, end);
    if (a < 0 || a >= MAXIDX || !ep || b < 0 || b >= MAXIDX) return -1;  // damaged line
    t[a][ep - edge_name] = (int8_t)b;
    links++;
  }
  memcpy(adj, t, sizeof adj);
  return links;
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

// Put the cursor on the exit edge (centre of the edge, or the given point), then push across it.
static void push_step(int e, uint16_t px, uint16_t py) {
  if (e == E_L) px = 0;
  if (e == E_R) px = 32767;
  if (e == E_T) py = 0;
  if (e == E_B) py = 32767;
  pq_abs(px, py, true, true);
  pq_rel((int16_t)(dir_x[e] * PUSH), (int16_t)(dir_y[e] * PUSH), 0, true);
}

// Put the cursor on display t when the Pico does not know which display holds it (after
// boot, after a return from another computer). This works when the learned displays of
// t's group are one row or one column: n - 1 pushes toward one end put the cursor on
// that end from any display (macOS stops the cursor at the outer edge), then the path
// goes from that end to t. Returns the number of steps, or -1 if not possible.
static int home(int t) {
  uint8_t list[MAXIDX], n = 0;
  bool seen[MAXIDX] = {false};
  int axis = -1;  // 0 = left/right, 1 = top/bottom
  seen[t] = true;
  list[n++] = (uint8_t)t;
  for (int i = 0; i < n; i++)
    for (int e = 0; e < 4; e++) {
      int b = adj[list[i]][e];
      if (b < 0) continue;
      if (axis < 0) axis = e >> 1;
      if (axis != e >> 1) return -1;  // not one row or one column
      if (!seen[b]) { seen[b] = true; list[n++] = (uint8_t)b; }
    }
  if (n < 2) return -1;

  int e = adj[t][axis * 2] < 0 ? axis * 2 : axis * 2 + 1;  // t is at the end past edge e, if possible
  int end = t, count = 1;
  while (adj[end][e] >= 0 && count <= n) { end = adj[end][e]; count++; }
  for (int d = t; adj[d][e ^ 1] >= 0 && count <= n; d = adj[d][e ^ 1]) count++;
  if (count != n) return -1;  // not a simple chain

  for (int i = 0; i < n - 1; i++) push_step(e, 16384, 16384);
  int steps = n - 1;
  for (int d = end; d != t; d = adj[d][e ^ 1]) { push_step(e ^ 1, 16384, 16384); steps++; }
  return steps;
}

static void glide_report(uint16_t x, uint16_t y, int idx) {
  uint32_t now = ms(), gap = now - last_glide_ms;
  bool rehome = cur_disp < 0;  // first report after boot
  glide_reports++;
  last_glide_ms = now;

  if (last_idx >= 0 && idx != last_idx) {
    // A real crossing: exit near an edge, entry near the opposite edge. A return from
    // another host enters on the same side as it left, fails this test, and must not
    // create an edge. No time limit: the user can stop at the edge before the crossing.
    int e = crossing_edge(lx, ly, x, y);
    bool real = e >= 0;
    if (!real) { uint16_t d; e = nearest_edge(lx, ly, &d); }  // for the log line only
    ev("%s %lu %d->%d edge=%c exit=(%u,%u) entry=(%u,%u) gap=%lu\r\n", real ? "XING" : "REENTRY",
       (unsigned long)now, last_idx, idx, edge_name[e], lx, ly, x, y, (unsigned long)gap);
    if (real) {
      int8_t before[MAXIDX][4];
      memcpy(before, adj, sizeof adj);
      learn_edge(last_idx, e, idx);
      crossings++;
      if (memcmp(before, adj, sizeof adj) != 0) log_edges();
    } else {
      reentries++;
      rehome = true;
    }
  }

  // After boot or a return, the Mac display that holds the cursor is not known.
  bool moved = false;
  if (rehome) {
    int n = home(idx);
    if (n >= 0) {
      homes++;
      moved = true;
      cur_disp = idx;
      ev("HOME %lu ->%d steps=%d\r\n", (unsigned long)now, idx, n);
    } else if (cur_disp < 0) {
      guesses++;
      cur_disp = idx;  // assume that the cursor is on this display
      ev("GUESS %lu ->%d\r\n", (unsigned long)now, idx);
    }
  }

  if (idx != cur_disp) {
    uint8_t edges[MAXIDX];
    int n = find_path(cur_disp, idx, edges);
    if (n > 0) {
      int d = cur_disp;
      for (int i = 0; i < n; i++) {
        int e = edges[i];
        if (d == last_idx) push_step(e, lx, ly);  // the real exit point
        else push_step(e, 16384, 16384);
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

// Mac side. Suspend = the Mac sleeps or turns off the port.
void tud_mount_cb(void) { ev("MAC %lu mounted\r\n", (unsigned long)ms()); }
void tud_umount_cb(void) { ev("MAC %lu unmounted\r\n", (unsigned long)ms()); }
void tud_suspend_cb(bool remote_wakeup_en) { ev("MAC %lu suspend rwake=%d\r\n", (unsigned long)ms(), remote_wakeup_en); }
void tud_resume_cb(void) { ev("MAC %lu resume\r\n", (unsigned long)ms()); }

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

  // Flash log: erases this boot's slot (about 0.6 s), so do it before USB starts.
  uint32_t prev_len, seq;
  const char *prev = flog_init(FW_VERSION, &prev_len, &seq);
  int restored = prev ? restore_edges(prev, prev_len) : -1;

  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
  pio_cfg.pin_dp = PIO_USB_DP_PIN;
  tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
  // Report protocol, not boot protocol: NKRO keyboard, 16-bit mouse, vendor bytes.
  tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);

  ev("BOOT glide-pico " FW_VERSION " seq=%lu dp=GP%u dm=GP%u push=%d restored=%d\r\n", (unsigned long)seq,
     PIO_USB_DP_PIN, PIO_USB_DP_PIN + 1, PUSH, restored);
  if (restored > 0) log_edges();  // so that each slot starts with the edges in use
  tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO};
  tusb_init(BOARD_TUD_RHPORT, &dev_init);
  tusb_rhport_init_t host_init = {.role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_AUTO};
  tusb_init(BOARD_TUH_RHPORT, &host_init);

  uint32_t next_hb = 0, next_stat = STAT_MS, stat_glide = 0;
  bool last_port = false;
  while (true) {
    tud_task();
    tuh_task();
    kbd_task();
    pointer_task();
    log_task();

    uint32_t now = ms();
    flog_task(now, now - last_glide_ms >= 300);
    bool port = hcd_port_connect_status(BOARD_TUH_RHPORT);
    if (port != last_port) {
      ev("PORT %lu %s\r\n", (unsigned long)now, port ? "connected" : "disconnected");
      last_port = port;
    }
    uint8_t mounted = mounted_count();
    led_task(now, port, mounted);
    if ((int32_t)(now - next_hb) >= 0) {
      next_hb = now + 2000;
      out("HB %lu port=%d mounted=%u mac=%d cur=%d glide=%lu xing=%lu reentry=%lu home=%lu move=%lu nopath=%lu drops=%lu/%lu flash=%lu\r\n",
          (unsigned long)now, port, mounted, tud_mounted(), cur_disp, (unsigned long)glide_reports,
          (unsigned long)crossings, (unsigned long)reentries, (unsigned long)homes, (unsigned long)moves, (unsigned long)nopath,
          (unsigned long)kq_drops, (unsigned long)pq_drops, (unsigned long)flog_used());
    }
    if ((int32_t)(now - next_stat) >= 0) {  // only after use, so an idle Pico does not fill its slot
      next_stat = now + STAT_MS;
      if (glide_reports != stat_glide) {
        stat_glide = glide_reports;
        ev("STAT %lu glide=%lu xing=%lu reentry=%lu home=%lu guess=%lu move=%lu nopath=%lu drops=%lu/%lu flash=%lu lost=%lu\r\n",
           (unsigned long)now, (unsigned long)glide_reports, (unsigned long)crossings, (unsigned long)reentries,
           (unsigned long)homes, (unsigned long)guesses, (unsigned long)moves, (unsigned long)nopath,
           (unsigned long)kq_drops, (unsigned long)pq_drops, (unsigned long)flog_used(), (unsigned long)flog_lost());
      }
    }
#if CFG_TUD_CDC
    tud_cdc_write_flush();
#endif
  }
}
