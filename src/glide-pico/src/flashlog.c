// flashlog: event log in the Pico's own flash. See flashlog.h.
#include <string.h>
#include "flashlog.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/platform.h"

extern char __flash_binary_end;

#define QSIZE 8192u  // RAM queue for text that is not yet in flash

static char q[QSIZE];
static volatile uint32_t q_head, q_tail;  // free-running counts
static bool enabled;
static uint32_t slot_off;                 // flash offset of this boot's slot
static uint32_t page_off;                 // flash offset of the page in page_buf
static uint8_t page_buf[FLASH_PAGE_SIZE];
static uint32_t page_fill, page_done;     // bytes in page_buf, bytes of them in flash
static uint32_t first_unflushed_ms, lost;
static bool have_unflushed;

static const uint8_t *xip(uint32_t off) { return (const uint8_t *)(XIP_BASE + off); }

static void program_page(void) {
  uint32_t irq = save_and_disable_interrupts();
  flash_range_program(page_off, page_buf, FLASH_PAGE_SIZE);
  restore_interrupts(irq);
  page_done = page_fill;
}

const char *flog_init(const char *fw, uint32_t *prev_len, uint32_t *seq) {
  *prev_len = 0;
  *seq = 0;
  if ((uintptr_t)&__flash_binary_end - XIP_BASE > FLOG_BASE) return NULL;  // no room

  int newest = -1;
  uint32_t newest_seq = 0;
  for (int i = 0; i < FLOG_SLOTS; i++) {
    const flog_header_t *h = (const flog_header_t *)xip(FLOG_BASE + i * FLOG_SLOT_SIZE);
    if (h->magic == FLOG_MAGIC && h->version == FLOG_VERSION && h->slot_size == FLOG_SLOT_SIZE &&
        (newest < 0 || h->seq > newest_seq)) {
      newest = i;
      newest_seq = h->seq;
    }
  }
  const char *prev = NULL;
  if (newest >= 0) {
    prev = (const char *)xip(FLOG_BASE + newest * FLOG_SLOT_SIZE + FLASH_PAGE_SIZE);
    const char *end = memchr(prev, 0xFF, FLOG_SLOT_SIZE - FLASH_PAGE_SIZE);
    *prev_len = end ? (uint32_t)(end - prev) : FLOG_SLOT_SIZE - FLASH_PAGE_SIZE;
  }

  int slot = newest < 0 ? 0 : (newest + 1) % FLOG_SLOTS;
  slot_off = FLOG_BASE + slot * FLOG_SLOT_SIZE;
  *seq = newest_seq + 1;

  flog_header_t h = {.magic = FLOG_MAGIC, .version = FLOG_VERSION, .seq = *seq, .slot_size = FLOG_SLOT_SIZE};
  strncpy(h.fw, fw, sizeof h.fw - 1);
  memset(page_buf, 0xFF, sizeof page_buf);
  memcpy(page_buf, &h, sizeof h);

  uint32_t irq = save_and_disable_interrupts();
  flash_range_erase(slot_off, FLOG_SLOT_SIZE);  // 64 KiB block erases, about 0.6 s in total
  flash_range_program(slot_off, page_buf, FLASH_PAGE_SIZE);
  restore_interrupts(irq);

  page_off = slot_off + FLASH_PAGE_SIZE;
  memset(page_buf, 0xFF, sizeof page_buf);
  page_fill = page_done = 0;
  enabled = true;
  return prev;
}

void flog_append(const char *s, uint32_t n) {
  uint32_t irq = save_and_disable_interrupts();
  if (q_tail - q_head + n <= QSIZE) {
    for (uint32_t i = 0; i < n; i++) q[(q_tail + i) % QSIZE] = s[i];
    q_tail += n;
  } else {
    lost += n;
  }
  restore_interrupts(irq);
}

void flog_task(uint32_t now_ms, bool idle) {
  if (!enabled) { q_head = q_tail; return; }

  // Move queued text into the page buffer. Program a full page, then start the next.
  while (q_head != q_tail) {
    if (page_fill == FLASH_PAGE_SIZE) {
      // Programming stops all interrupts for about 1 ms. Wait for an idle pointer,
      // unless the queue is nearly full.
      if (!idle && q_tail - q_head < QSIZE * 3 / 4) return;
      program_page();
      have_unflushed = false;
      page_off += FLASH_PAGE_SIZE;
      memset(page_buf, 0xFF, sizeof page_buf);
      page_fill = page_done = 0;
      if (page_off >= slot_off + FLOG_SLOT_SIZE) {  // slot full: keep the first part
        enabled = false;
        lost += q_tail - q_head;
        q_head = q_tail;
        return;
      }
    }
    page_buf[page_fill++] = (uint8_t)q[q_head % QSIZE];
    q_head++;
    if (!have_unflushed) { have_unflushed = true; first_unflushed_ms = now_ms; }
  }

  // Program a part-filled page 2 s after its first unsaved byte, so that a power-off
  // loses little. Programming the page again with the same bytes does not change them.
  if (have_unflushed && page_fill > page_done && idle && now_ms - first_unflushed_ms >= 2000) {
    program_page();
    have_unflushed = false;
  }
}

uint32_t flog_used(void) { return enabled ? page_off - slot_off + page_fill : FLOG_SLOT_SIZE; }
uint32_t flog_lost(void) { return lost; }
