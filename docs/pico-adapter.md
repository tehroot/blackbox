# Pico adapter: multi-monitor Glide and Switch with no software on the Mac

A Raspberry Pi Pico (RP2040) goes between the KVM computer port and the Mac. The Mac
sees a plain USB keyboard and mouse. It needs no software, no permissions and no admin
rights. This is the solution for the locked-down work Mac.

Firmware version: **0.5.1** (2026-10-08). Source: `src/glide-pico/`.

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

The Pico tests all four edges, not only the nearest edge of the exit point (version
0.5.1). At a corner, the nearest edge can be the side edge: exit `(0,399)` and entry
`(0,32486)` is a crossing of the top edge, although the exit is nearer to the left edge.

A return from another computer enters on the same side as it left, so it fails this
test and does not create an edge. There is no time limit, because you can stop at an
edge before you cross it. A new edge between two indexes removes older edges between
them, and older edges that the new edge replaces.

### 5.4 Homing (version 0.5)

After a boot, and after a return from another computer, the Pico does not know which
Mac display holds the cursor. Version 0.4 assumed the display of the last index. When
this was not true (the edges were lost at power-off, or the trackpad had moved the
cursor), the first move went the wrong way, and the cursor seemed to wrap on one display.

Version 0.5 puts the cursor on the correct display without this assumption:

1. The learned displays must be one row or one column (for example, one display above
   the other). If not, the Pico uses the version 0.4 method.
2. The Pico does n − 1 pushes toward one end of the row or column (n = number of
   displays). macOS stops the cursor at the outer edge, so after these pushes the cursor
   is on that end display, from any start display.
3. The Pico then does one push for each display from that end to the target display.
4. Each push starts at the centre of the edge, not at the corner, so the two displays
   touch at that point.

For two displays, one above the other, this is one push. The `HOME` log line shows it.
If no edges are known, the Pico assumes the display of the first index (`GUESS`), and
the first crossing teaches the edge.

### 5.5 Flash log and saved edges (version 0.5)

The event lines also go to the Pico's own flash (`flashlog.c`, `flashlog.h`). This works
on the work Mac too: nothing is added on the Mac side.

- Area: flash offset `0x080000`–`0x200000` (1.5 MiB), 6 slots of 256 KiB. The firmware
  is about 72 KiB, below `0x080000`. A check at boot turns the log off if the firmware
  grows into the area.
- Each boot uses the next slot, so the logs of the 5 previous boots stay. The Pico
  erases the slot at boot, before USB starts (about 0.6 s).
- At run time the Pico only programs 256-byte pages. Programming stops all interrupts
  for about 1 ms (3 ms maximum), so the Pico programs only when the KVM pointer has not
  moved for 300 ms. A part-filled page is programmed 2 s after its first byte, so a
  power-off loses at most 2 s of lines.
- A slot holds about 2,500 crossings. When a slot is full, the lines after that point
  are lost (`lost=` in the STAT line). The next boot starts a new slot.
- A UF2 copy does not erase the area: the logs and the edges stay after a new flash.

**Saved edges:** each change of the learned edges writes an `EDGES` line. At boot, the
Pico reads the last `EDGES` line of the previous boot's slot and restores the edges.
Then it writes them again at the start of the new slot.

Constants in `main.c`: `PUSH` 40, `GAP_US` 2000, `EDGE_NEAR` 3000, `MAXIDX` 16,
`STAT_MS` 600000.

## 6. Log

### 6.1 Flash log (both builds)

Read it on the personal Mac. You do not need software or permissions on the work Mac.

1. Disconnect the Pico from the Mac. The KVM cable can stay in the Pico.
2. Hold BOOTSEL, connect the Pico to the personal Mac, release BOOTSEL.
3. `tools/pico-log.sh`
   - It saves the log area to `logs/pico-<date>.bin` and the text to
     `logs/pico-<date>.txt` (`logs/` is not in git), and prints the text.
   - Then it restarts the firmware. Disconnect the Pico and connect it to the work Mac.
   - `--stay` keeps the Pico in BOOTSEL mode, for example to copy a new UF2 next.

`tools/pico-log.sh --clear` erases the log area: all logs **and the saved edges**. Use it
after a large change of the KVM layout, if the old edges cause wrong moves. Normally
you do not need it, because a new crossing replaces a conflicting edge.

The script needs picotool with USB support. `tools/setup-pico-toolchain.sh` builds it
into `~/pico/picotool` (no admin rights).

Timestamps are milliseconds since the boot of that slot. The Pico has no clock. Use
the `MAC ... suspend` and `MAC ... resume` lines to find sleep periods of the Mac.

### 6.2 Serial log (development build)

Open the serial port, for example with `cat /dev/cu.usbmodem*`. The Pico keeps event
lines from boot (16 KiB) and sends them again each time a terminal opens the port.

### 6.3 Lines

| Line | Meaning |
|---|---|
| `BOOT glide-pico 0.5 seq=n ... restored=k` | start; boot number; number of edge links restored from flash (−1: none) |
| `EDGES 0T1 1B0` | learned edges: index 1 is past the top edge of index 0, index 0 is past the bottom edge of index 1 |
| `MAC <ms> mounted / unmounted / suspend / resume` | state of the Mac side (suspend = the Mac sleeps) |
| `PORT <ms> connected` | KVM detected on D+/D− |
| `MOUNT` / `HID ... role=...` | KVM devices and the role of each HID interface |
| `XING <ms> a->b edge=E exit=(x,y) entry=(x,y) gap=<ms>` | crossing; edge learned |
| `REENTRY ...` | index change that is not a crossing (return from another computer) |
| `MOVE <ms> a->b steps=n` | display change sent to the Mac |
| `HOME <ms> ->b steps=n` | homing to the display of index b (section 5.4) |
| `GUESS <ms> ->b` | no edges known: the Pico assumes that the cursor is on the display of index b |
| `NOPATH <ms> a->b` | no learned path; no display change sent |
| `STAT <ms> glide xing reentry home guess move nopath drops flash lost` | counters, every 10 minutes of use (flash = bytes in this slot, lost = bytes not written) |
| `HB <ms> ...` | heartbeat every 2 s (serial log only) |

To see TinyUSB enumeration details, set `CFG_TUSB_DEBUG` to 2 in `tusb_config.h`.

## 7. Limits

- **Homing needs one row or one column.** For other layouts (for example an L shape),
  the Pico uses the version 0.4 method after a boot or a return: it assumes the last
  display, and the first crossing corrects a wrong assumption.
- **No edges at the first boot.** On a new Pico, or after `--clear`, the Pico assumes
  the display of the first index (`GUESS`) until the first crossing.
- **Trackpad of the Mac.** If you move the cursor to the other display with the Mac's
  own trackpad while the KVM mouse is on this Mac, the KVM mouse stays on that display
  until the next crossing. After a return from another computer, homing corrects it.
  A return onto the **same index** as the exit is not a change of index, so it gets no
  homing.
- **Homing is visible.** The cursor goes to the centre of an edge for about 4 ms. If the
  Dock hides automatically on that edge, it can appear.
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
| Cursor stays on one display; at an edge it jumps to the other side of the same display | Wrong or missing edge, or the Pico assumed the wrong display. Read the flash log (section 6.1): look for `NOPATH`, `GUESS`, and the `EDGES` lines. Disconnecting the Pico does not clear the edges in version 0.5: use `tools/pico-log.sh --clear`. |
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
| 7 | (2026-10-07, work Mac, 0.4) After some starts, the cursor wrapped on one display until the first crossing | The edges were in RAM and lost at power-off. The Pico assumed that the cursor was on the display of the first index. Probable cause, not confirmed with a log. | Version 0.5: edges saved in flash, homing after boot and return, flash log |
| 8 | (2026-10-08, flash log, 0.5) A crossing near a corner was logged as `REENTRY`, then `NOPATH` because no edge was known yet | The test used only the nearest edge of the exit point. At `(0,399)` that is the left edge, not the top edge. Present since 0.4. Probably the main cause of fault 7. | Version 0.5.1: all four edges are tested |
| 9 | (2026-10-08, flash log, 0.5) Timestamps started again from 0, and `STAT` lines stopped after 71.6 minutes | `time_us_32() / 1000` wraps at 4,294,967 ms | Version 0.5.1: `to_ms_since_boot()` (wraps after 49.7 days) |

## 10. Test record

- 2026-10-06, personal Mac, version 0.2: 22 crossings, 22 display changes, 0 `NOPATH`,
  0 lost reports (`data/pico-adapter-0.2-crossings.log`). Keyboard, Caps Lock, buttons,
  wheel and drags worked.
- 2026-10-06, work build 0.3: same result on the personal Mac and on the work Mac (2
  displays, one above the other). Then fault 6 occurred.
- 2026-10-06, work build 0.4 on the work Mac: crossings, returns from the personal Mac
  onto either display, and crossings after the return worked.

- 2026-10-07, version 0.5: built without warnings. The edge restore, homing and
  report sequences were tested on the Mac with the firmware logic and stub headers
  (two displays one above the other, three in a row, L shape, a damaged `EDGES` line).
  `tools/pico-log-decode.py` was tested with a constructed dump.
- 2026-10-08, version 0.5 on the Pico: flash log read with `tools/pico-log.sh`. One boot
  of about 2 hours: 56 crossings, 0 lost reports, 0 lost log bytes, 3.5 KB of log. The
  `EDGES` line was written. The log showed faults 8 and 9. The next boot restored the
  edges from flash (`restored=2`).
- 2026-10-08, version 0.5.1: the new crossing test gives the correct edge for all 58
  index changes of that log, and rejects constructed returns from another computer.
- 2026-10-08, version 0.5.1 on the Pico: two boots restored the edges (`restored=2`)
  and did homing at the first report. 28 crossings, all recognized, 0 `NOPATH`. Log:
  `data/pico-adapter-0.5-flash-log.txt` (boots 1–3: 0.5, boots 4–5: 0.5.1).

## 11. Possible improvements

- Homing for layouts that are not one row or one column.
- Strain relief or a case. The solder joints carry the USB signals.
- Support other keyboards. The Pico cannot copy a descriptor at run time, because the
  Mac enumerates the Pico before the Pico knows the KVM keyboard. A list of known
  descriptors, or a conversion to a standard keyboard report, would work.
