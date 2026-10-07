#ifndef USB_DESCRIPTORS_H_
#define USB_DESCRIPTORS_H_

#include <stdint.h>

// Device-side HID instances (same order as the HID interfaces).
enum { HID_KBD = 0, HID_MOUSE = 1, HID_ABS = 2 };

#define KBD_DESC_LEN 109
extern uint8_t const desc_hid_keyboard[KBD_DESC_LEN];

#endif
