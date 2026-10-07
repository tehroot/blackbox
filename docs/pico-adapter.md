# Pico adapter: multi-monitor Glide and Switch with no software on the Mac

A Raspberry Pi Pico (RP2040) goes between the KVM computer port and the Mac. The Mac
sees a plain USB keyboard and mouse. It needs no software, no permissions and no admin
rights. This is the solution for the locked-down work Mac.

Firmware version: **0.4** (2026-10-06). Source: `src/glide-pico/`.

## 1. Hardware

| Part | Note |
|---|---|
| Raspberry Pi Pico (RP2040) | Pico W: not tested. Its LED is on the wireless chip, not on GP25, so the LED code needs changes. |
| USB cable with a male USB-B plug | Cut a spare A-to-B cable. Keep the B plug and a short cable. |
| Micro-USB cable | Pico to Mac |
| 2 × 22–27 Ω resistors | Optional, in series with D+ and D−, near the Pico |

### Wiring (KVM side)

| Wire (standard color) | USB-B pin | Pico |
|---|---|---|
| Red, VBUS | 1 | VBUS, pin 40 |
| White, D− | 2 | GP21, pin 27 |
| Green, D+ | 3 | GP20, pin 26 |
| Black, GND | 4 | GND, pin 28 |

- Check the wire colors with a meter. Some cables use other colors.
- D− must be on the GPIO after D+ (PIO-USB rule). Other pairs work if you change
  `PIO_USB_DP_PIN` in `main.c`. GP16/GP17 with GND on pin 23 is a good alternative.
- Do not connect the KVM VBUS to VSYS (pin 39) or 3V3 (pin 36).
- Keep the cable short (tens of centimeters). PIO-USB is a software USB host and is
  sensitive to signal quality.
- The B plug goes directly into the KVM computer port. The KVM port is a female USB-B
  port.

### Connections

```
KVM computer port ──USB-B plug──► Pico GP20/GP21 (PIO-USB host)
                                   Pico micro-USB (native USB device) ──► Mac
```

**Connect the Pico to a direct port of the Mac.** On the personal Mac, a port of an
external Genesys hub stopped working after some enumeration failures (macOS:
"persistent enumeration failures", "disabling port"). The Pico itself had no fault.

## 2. Build

```sh
tools/setup-pico-toolchain.sh   # one time: Arm GCC 14.2, pico-sdk 2.3.1, TinyUSB deps → ~/pico
src/glide-pico/build.sh         # builds both images, copies them to the repository root
```

Baseline versions: pico-sdk 2.3.1 (`079c6f3`), TinyUSB `86ad6e5`, Pico-PIO-USB
`fe9133f`, Arm GNU Toolchain 14.2.rel1, CMake 4.4.2, macOS 15.7.7 (arm64).

| Image | USB ID | Content | Flash method |
|---|---|---|---|
| `glide_pico.uf2` (development) | `CAFE:4A02` | keyboard, mouse, pointer **and a CDC serial port** (log, remote reboot) | BOOTSEL, or remote (see below) |
| `glide_pico_work.uf2` (work) | `CAFE:4A03` | keyboard, mouse, pointer only | BOOTSEL only |

`0xCAFE` is a development vendor ID (from TinyUSB examples). It is not registered.
Do not distribute devices with it.

## 3. Flash

**BOOTSEL:** hold BOOTSEL, connect the Pico to a Mac, release BOOTSEL. The `RPI-RP2`
drive appears. Copy the `.uf2` file to it. The Pico restarts with the new firmware.
Do this on the personal Mac: company policy can block USB drives on the work Mac.
The KVM cable can stay in the Pico.

**Remote (development build only):** open the serial port at 1200 baud. The Pico
restarts into the boot loader, and then you copy the file:

```sh
stty -f /dev/cu.usbmodem* 1200
# wait for /Volumes/RPI-RP2, then:
cp glide_pico.uf2 /Volumes/RPI-RP2/
```

## 4. LED

| LED | Meaning | Action |
|---|---|---|
| Short blink, 1 per second | Firmware runs. Nothing detected on the KVM side. | Check D+/D−/GND/VBUS wiring. |
| Blink, 5 per second | KVM detected, enumeration not complete | Signal problem: swapped D+/D−, long wires, bad solder joint |
| On | KVM devices enumerated | Correct |
| On with fast flicker | Position reports arrive | Correct, while the mouse moves on this computer |

## 5. Design

### 5.1 KVM side (USB host, PIO-USB on core 0, rhport 1)

- Two hubs in series (`05E3:0610` → `21D1:0010`), so `CFG_TUH_HUB` is 3.
- HID report protocol for all interfaces (`tuh_hid_set_default_protocol`), so the NKRO
  keyboard and the 16-bit mouse values arrive.
- Role of each HID interface:

| Test | Role |
|---|---|
| VID/PID `21D1:0001` | glide (position) |
| Report descriptor equal to the Launch NKRO descriptor (109 bytes) | keyboard |
| Interface protocol = mouse | mouse (buttons, wheel; X/Y also pass, for manual mode) |
| All other interfaces | ignored |

### 5.2 Mac side (USB device, native USB, rhport 0)

| Interface | Report descriptor | Source of the reports |
|---|---|---|
| 0 Keyboard | exact copy of the Launch interface 3 (report IDs 3, 4, 5) | keyboard reports without change; Caps Lock LED goes back to the keyboard |
| 1 Mouse | relative: 5 buttons, X/Y int16, wheel, AC pan | real-mouse buttons and wheel; the "push" movements |
| 2 Pointer | absolute X/Y 0..32767, no buttons | glide position without the index byte |
| 3–4 CDC | development build only | log |

Queues:

- Keyboard: 16 reports. No key report is lost.
- Pointer: 32 actions, sent **strictly in order** across the mouse and pointer
  interfaces. The next action goes only after the Mac has taken the previous one
  (`tud_hid_report_complete_cb`). A newer absolute position replaces an unsent older
  one, so the queue does not fill while the mouse moves. If no completion arrives in
  50 ms, the queue continues.

### 5.3 Display change ("method 2")

macOS maps an absolute position onto the display that holds the cursor (measured). So:

1. While the monitor index stays the same, the Pico sends the absolute X/Y without
   change. The position on that display is exact.
2. When the index changes, the Pico:
   1. sends an absolute position on the **exit edge** of the current display;
   2. after 2 ms, sends a relative movement of 40 counts across that edge ("push").
      macOS moves the cursor onto the next display;
   3. after 2 ms, continues with absolute positions, which now apply to the new display.
3. For a change between displays that are not adjacent, the Pico finds a path of
   edges (breadth-first search) and does one push for each edge.

The Pico **learns** which index is past which edge from the crossings that the KVM
makes. It needs no config for the Mac. A crossing counts only if (version 0.4):

- the last report before the change is within 3000 units of an edge (exit edge),
- the first report after the change is within 3000 units of the **opposite** edge.

A return from another computer enters on the same side as it left, so it fails this
test and does not create an edge. There is no time limit, because you can stop at an
edge before you cross it. A new edge between two indexes removes older edges between
them, and older edges that the new edge replaces.

Constants in `main.c`: `PUSH` 40, `GAP_US` 2000, `EDGE_NEAR` 3000, `MAXIDX` 16.

## 6. Log (development build)

Open the serial port, for example with `cat /dev/cu.usbmodem*`. The Pico keeps event
lines from boot (16 KiB) and sends them again each time a terminal opens the port.

| Line | Meaning |
|---|---|
| `BOOT glide-pico 0.4 ...` | start |
| `PORT <ms> connected` | KVM detected on D+/D− |
| `MOUNT` / `HID ... role=...` | KVM devices and the role of each HID interface |
| `XING <ms> a->b edge=E exit=(x,y) entry=(x,y) gap=<ms>` | crossing; edge learned |
| `REENTRY ...` | index change that is not a crossing (return from another computer) |
| `MOVE <ms> a->b steps=n` | display change sent to the Mac |
| `NOPATH <ms> a->b` | no learned path; no display change sent |
| `HB <ms> port mounted mac cur glide xing reentry move nopath drops=k/p` | heartbeat every 2 s |

To see TinyUSB enumeration details, set `CFG_TUSB_DEBUG` to 2 in `tusb_config.h`.

## 7. Limits

- **Learned edges are in RAM.** They are lost at power-off. The first crossing of each
  edge teaches it again, and that crossing works.
- **First entry after power-on.** The Pico assumes that the cursor is on the display
  of the first index it sees. If that is not true, the first crossing corrects it.
- **Trackpad of the Mac.** If you move the cursor to the other display with the Mac's
  own trackpad, the KVM mouse stays on that display until the next crossing.
- **Display arrangement.** The push goes across the exit edge. The Mac must have the
  next display on that side, and it must touch that part of the edge. The KVM layout
  and the macOS arrangement must agree.
- **Keyboard type.** The keyboard interface is a copy of the System76 Launch. Another
  keyboard on the KVM gets the role "ignored" and its keys do not arrive. To support
  it, copy its descriptor into `usb_descriptors.c`.
- **Company security.** The Mac sees a new USB keyboard and mouse. Security software
  can record new USB devices. The work build has no serial port, to keep the device
  simple.

## 8. Troubleshooting

| Symptom | Cause and action |
|---|---|
| LED short blink with the KVM connected | No device detected: wiring, VBUS, or the KVM port is not active |
| LED 5 Hz blink | Enumeration fails: signal quality. Use the development build and set `CFG_TUSB_DEBUG` 2. |
| Mac does not see the Pico; macOS log has `failed to address device, disabling port` | Port problem. Use a direct port of the Mac, or another cable. |
| Cursor stays on one display; at an edge it jumps to the other side of the same display | Wrong or missing edge. Version 0.3 and earlier learned false edges at a return from another computer. Disconnect and connect the Pico (clears RAM). With 0.4, check the log for `NOPATH`. |
| Keys do not arrive | Keyboard role not found (`role=ignored` in the log): see the keyboard limit above. |

## 9. Fault history (2026-10-06)

| # | Symptom | Cause | Correction |
|---|---|---|---|
| 1 | No device events in the log | Lines before the terminal opened the port were lost | Event log in RAM, sent again at each port open |
| 2 | D+ and D− both read 1 | PIO-USB inverts these inputs (`GPIO_OVERRIDE_INVERT`) | Heartbeat inverts the value back |
| 3 | `enum_request_set_addr: ASSERT FAILED` | `CFG_TUH_HUB 1`; the KVM has two hubs, so the second hub got no address | `CFG_TUH_HUB 3` |
| 4 | LED did not go on | The device count did not include hub addresses (and an edit made the loop body a comment) | Count up to `CFG_TUH_DEVICE_MAX + CFG_TUH_HUB` |
| 5 | Pico not found by the Mac, also in BOOTSEL mode | A port of an external hub was disabled by macOS | Use a direct Mac port |
| 6 | After some use, the cursor stayed on one display | A return from another computer onto a different display was learned as an edge (version 0.3) | Version 0.4: geometric crossing test, conflict removal |

## 10. Test record

- 2026-10-06, personal Mac, version 0.2: 22 crossings, 22 display changes, 0 `NOPATH`,
  0 lost reports (`data/pico-adapter-0.2-crossings.log`). Keyboard, Caps Lock, buttons,
  wheel and drags worked.
- 2026-10-06, work build 0.3: same result on the personal Mac and on the work Mac (2
  displays, one above the other). Then fault 6 occurred.
- 2026-10-06, work build 0.4 on the work Mac: crossings, returns from the personal Mac
  onto either display, and crossings after the return worked.

## 11. Possible improvements

- Keep the learned edges in flash, so they are known at power-on.
- Strain relief or a case. The solder joints carry the USB signals.
- Support other keyboards. The Pico cannot copy a descriptor at run time, because the
  Mac enumerates the Pico before the Pico knows the KVM keyboard. A list of known
  descriptors, or a conversion to a standard keyboard report, would work.
