#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

// Native USB (rhport 0) = device to the Mac. PIO-USB (rhport 1) = host to the KVM.
#define BOARD_TUD_RHPORT          0
#define BOARD_TUH_RHPORT          1

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS               OPT_OS_NONE
#endif
// TinyUSB debug log goes to the RAM event log in main.c (dbg_printf). Set
// CFG_TUSB_DEBUG to 2 for host enumeration details; device log stays off (level 3 > 2).
#define CFG_TUSB_DEBUG            0
#define CFG_TUH_LOG_LEVEL         2
#define CFG_TUD_LOG_LEVEL         3
#define CFG_TUSB_DEBUG_PRINTF     dbg_printf
int dbg_printf(const char *f, ...);

#define CFG_TUD_ENABLED           1
#define CFG_TUD_MAX_SPEED         OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE    64

#define CFG_TUH_ENABLED           1
#define CFG_TUH_MAX_SPEED         OPT_MODE_FULL_SPEED
#define CFG_TUH_RPI_PIO_USB       1

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN        __attribute__ ((aligned(4)))

//------------- Device: keyboard + relative mouse + absolute pointer + CDC log -------------
#define CFG_TUD_HID               3
#define CFG_TUD_HID_EP_BUFSIZE    64
#ifdef GLIDE_NO_CDC
#define CFG_TUD_CDC               0   // work build: keyboard + mouse + pointer only
#else
#define CFG_TUD_CDC               1
#endif
#define CFG_TUD_CDC_RX_BUFSIZE    64
#define CFG_TUD_CDC_TX_BUFSIZE    4096
#define CFG_TUD_CDC_EP_BUFSIZE    64

//------------- Host: KVM hubs + Glide and Switch mouse + keyboard + mouse -------------
#define CFG_TUH_ENUMERATION_BUFSIZE 512
#define CFG_TUH_HUB               3   // the KVM has two hubs in series (05E3:0610, 21D1:0010)
#define CFG_TUH_DEVICE_MAX        8
#define CFG_TUH_HID               16
#define CFG_TUH_HID_EPIN_BUFSIZE  64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64

#endif
