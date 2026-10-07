// Device-side descriptors: what the Mac sees.
//   interface 0  HID keyboard   (copy of the System76 Launch NKRO interface)
//   interface 1  HID mouse      (relative: buttons, X/Y, wheel, pan)
//   interface 2  HID pointer    (absolute X/Y, 0..32767)
//   interface 3-4 CDC serial log (not in the work build, GLIDE_NO_CDC)
#include <string.h>
#include "tusb.h"
#include "usb_descriptors.h"

#define USB_VID 0xCAFE  // development ID, not for distribution
#if CFG_TUD_CDC
#define USB_PID 0x4A02
#else
#define USB_PID 0x4A03  // separate ID for the work build, so macOS keeps the two apart
#endif

// System76 Launch (3384:0001) interface 3, read from the device through the KVM.
// Report IDs: 3 system control, 4 consumer control, 5 NKRO keyboard (+ LED output).
uint8_t const desc_hid_keyboard[KBD_DESC_LEN] = {
  0x05, 0x01, 0x09, 0x80, 0xa1, 0x01, 0x85, 0x03, 0x19, 0x01, 0x2a, 0xb7, 0x00, 0x15, 0x01, 0x26,
  0xb7, 0x00, 0x95, 0x01, 0x75, 0x10, 0x81, 0x00, 0xc0, 0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85,
  0x04, 0x19, 0x01, 0x2a, 0xa0, 0x02, 0x15, 0x01, 0x26, 0xa0, 0x02, 0x95, 0x01, 0x75, 0x10, 0x81,
  0x00, 0xc0, 0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x05, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
  0x15, 0x00, 0x25, 0x01, 0x95, 0x08, 0x75, 0x01, 0x81, 0x02, 0x05, 0x07, 0x19, 0x00, 0x29, 0xef,
  0x15, 0x00, 0x25, 0x01, 0x95, 0xf0, 0x75, 0x01, 0x81, 0x02, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
  0x95, 0x05, 0x75, 0x01, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0xc0,
};

// Relative mouse, no report ID: buttons(5 bits + 3 pad), X int16, Y int16, wheel int8, pan int8.
static uint8_t const desc_hid_mouse[] = {
  0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
  0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
  0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
  0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x02, 0x81, 0x06,
  0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
  0x05, 0x0c, 0x0a, 0x38, 0x02, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
  0xc0, 0xc0,
};

// Absolute pointer, no report ID, no buttons: X/Y uint16 0..32767. Same as the KVM's
// Glide and Switch mouse without its monitor byte.
static uint8_t const desc_hid_abs[] = {
  0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
  0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
  0xc0, 0xc0,
};

uint8_t const *tud_descriptor_device_cb(void) {
  static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
#if CFG_TUD_CDC
    .bDeviceClass       = TUSB_CLASS_MISC,     // CDC needs an interface association
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
#else
    .bDeviceClass       = 0,                   // class defined per interface (HID only)
    .bDeviceSubClass    = 0,
    .bDeviceProtocol    = 0,
#endif
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0003,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
  };
  return (uint8_t const *)&desc_device;
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
  switch (instance) {
    case HID_KBD:   return desc_hid_keyboard;
    case HID_MOUSE: return desc_hid_mouse;
    default:        return desc_hid_abs;
  }
}

#if CFG_TUD_CDC
enum { ITF_KBD = 0, ITF_MOUSE, ITF_ABS, ITF_CDC, ITF_CDC_DATA, ITF_TOTAL };
#else
enum { ITF_KBD = 0, ITF_MOUSE, ITF_ABS, ITF_TOTAL };
#endif
#define EP_KBD       0x81
#define EP_MOUSE     0x82
#define EP_ABS       0x83
#define EP_CDC_NOTIF 0x84
#define EP_CDC_OUT   0x05
#define EP_CDC_IN    0x85
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 3 * TUD_HID_DESC_LEN + CFG_TUD_CDC * TUD_CDC_DESC_LEN)

static uint8_t const desc_config[] = {
  TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
  TUD_HID_DESCRIPTOR(ITF_KBD, 4, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_keyboard), EP_KBD, 64, 1),
  TUD_HID_DESCRIPTOR(ITF_MOUSE, 5, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_mouse), EP_MOUSE, 16, 1),
  TUD_HID_DESCRIPTOR(ITF_ABS, 6, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_abs), EP_ABS, 16, 1),
#if CFG_TUD_CDC
  TUD_CDC_DESCRIPTOR(ITF_CDC, 7, EP_CDC_NOTIF, 8, EP_CDC_OUT, EP_CDC_IN, 64),
#endif
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) { (void)index; return desc_config; }

static char const *string_desc[] = {
  NULL,                  // 0: language (handled below)
  "glide-pico",          // 1: manufacturer
  "Glide Pico",          // 2: product
  "0003",                // 3: serial
  "Glide Pico Keyboard", // 4
  "Glide Pico Mouse",    // 5
  "Glide Pico Pointer",  // 6
  "Glide Pico Log",      // 7
};

static uint16_t _desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void)langid;
  size_t n;
  if (index == 0) {
    _desc_str[1] = 0x0409;
    n = 1;
  } else {
    if (index >= sizeof(string_desc) / sizeof(string_desc[0])) return NULL;
    const char *s = string_desc[index];
    n = strlen(s);
    if (n > 32) n = 32;
    for (size_t i = 0; i < n; i++) _desc_str[1 + i] = (uint16_t)s[i];
  }
  _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
  return _desc_str;
}
