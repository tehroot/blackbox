// flashlog: event log in the Pico's own flash, kept through power-off.
//
// The upper 1.5 MiB of the 2 MiB flash holds FLOG_SLOTS slots. Each boot uses the next
// slot, so the logs of the previous FLOG_SLOTS - 1 boots stay. A slot is a header page
// followed by log text. Unused flash reads 0xFF, so the text ends at the first 0xFF.
//
// Flash is erased only at boot, before USB starts. At run time the Pico only programs
// pages (about 1 ms each, interrupts off), and only when the KVM pointer is idle.
#ifndef FLASHLOG_H_
#define FLASHLOG_H_

#include <stdbool.h>
#include <stdint.h>

#define FLOG_BASE      0x080000u   // flash offset of slot 0 (firmware is below 512 KiB)
#define FLOG_SLOT_SIZE 0x040000u   // 256 KiB
#define FLOG_SLOTS     6
#define FLOG_MAGIC     0x474F4C47u // "GLOG"
#define FLOG_VERSION   1

typedef struct {
  uint32_t magic, version, seq, slot_size;
  char fw[16];
} flog_header_t;

// Finds the newest slot, returns its text (or NULL) for the caller to read old state,
// then erases the next slot and starts it. Call before USB starts.
const char *flog_init(const char *fw, uint32_t *prev_len, uint32_t *seq);
// Adds text to the RAM queue. Safe in interrupt context.
void flog_append(const char *s, uint32_t n);
// Programs queued text into flash. idle = the KVM pointer is not moving.
void flog_task(uint32_t now_ms, bool idle);
// Bytes written in this slot, and bytes lost (queue full or slot full).
uint32_t flog_used(void);
uint32_t flog_lost(void);

#endif
