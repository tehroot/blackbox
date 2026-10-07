# Black Box Freedom II: how Glide and Switch works

This document describes the KVM switch as we measured it on 2026-10-06. Each fact is
marked **measured** (seen in a capture, a log or a binary) or **inferred** (a
conclusion that agrees with the data but that we did not see directly).

## 1. The product

| Item | Value |
|---|---|
| Product | Black Box ServSwitch Freedom II, KV0004A-R2 (4 ports). KV0008A-R2 is the 8-port model. |
| Firmware | 2.10.8665 (2021-10-05). 2.05.8619 is also in the vendor files. |
| Origin | Adder "Free-Flow" design. The Windows driver PDB paths are `c:\winddk\freeflowdriver\...` (**measured**). |
| Configuration | The "Glide and Switch Configuration" application (Windows) sends a layout to the switch over HTTP (`/cgi-bin/config`, KV0004A-R2) or RS-232 (KV0004A). |

## 2. Principle: no software coordination

A software KVM (Synergy, Barrier, Deskflow) runs software on each computer. The
software reports the screen size and the cursor position, and it injects events with
operating system APIs. It is a closed loop, and it needs a network path between the
computers.

The Freedom II does all coordination inside the switch:

```
real mouse + keyboard ──► KVM (USB host to them)
                           │ internal cursor on the uploaded layout canvas
                           │ (all screens of all computers)
          ┌────────────────┼────────────────┐
          ▼                ▼                ▼
   computer port 1   computer port 2  ...   each port: a hub with emulated devices
                                              - absolute pointer 21D1:0001 (position)
                                              - copy of the real mouse (buttons, wheel)
                                              - copy of the real keyboard
```

1. **The KVM owns the input.** The real mouse and keyboard connect to the KVM. The KVM
   reads the relative movement of the real mouse (**measured**: the KVM is the USB host
   of these devices).
2. **The KVM keeps its own cursor.** It adds the movement to an internal position on
   the layout canvas. It applies its own acceleration (the "Mouse" parameter of the
   configuration application) (**inferred**).
3. **Absolute output.** Each computer gets an absolute pointer. An absolute position
   tells the computer where the cursor must be, so the KVM does not need to know the
   pointer acceleration or the real cursor position of the computer. It is an open
   loop. Relative output cannot work this way, because the acceleration on the
   computer changes the result and the KVM cannot read it back.
4. **Edge switching.** When the internal cursor goes into a screen of a different
   computer, the KVM sends the position, the keys and the buttons to that port.
5. **Separate streams** (**measured**). The position comes only from the absolute
   pointer. The copy of the real mouse sends buttons and wheel, and its X/Y values are
   always 0 in glide mode. So the computer gets no relative movement that conflicts
   with the absolute position.
6. **Fixed hubs** (**measured**). Each computer port always presents all devices. A
   computer does not enumerate again at a switch, so the switch is immediate. Only the
   active port gets reports.
7. **Parking.** When the cursor leaves a computer, the KVM moves the cursor of that
   computer to the "mouse parking" position of the configuration.

### Comparison

| | Freedom II | Synergy and similar |
|---|---|---|
| Software on the computers | None (except Windows multi-monitor) | Required |
| Network between computers | None. The computers cannot communicate. | Required |
| Locked-down computers | Works | Needs permissions |
| Real cursor position known | No. The KVM assumes that the computer follows. | Yes |
| Layout | Manual. It must agree with the display arrangement of each computer. | Automatic |
| Clipboard | No. Firmware 2.10 adds copy and paste only with a driver on each computer. | Yes |

## 3. USB structure of one computer port (measured)

```
05E3:0610  Genesys Logic USB 2.0 hub (bcdDevice 0x3298, self-powered, 4 ports)
└─ 21D1:0010  KVM hub (USB 1.1, full speed)
   ├─ 21D1:0001  "Glide and Switch USB Mouse" (Black Box Network Services)
   ├─ 3384:0001  copy of the System76 Launch keyboard (same name and descriptors)
   └─ 04A5:8002  copy of the BenQ ZOWIE mouse (same name and descriptors)
```

The keyboard and mouse copies have the names, IDs and descriptors of the real
devices. Thus the KVM probably clones the descriptors of the devices connected to its
console ports (**inferred**).

A USB host that supports only one hub level cannot use this port. The Pico adapter
needed `CFG_TUH_HUB` ≥ 2 (see `pico-adapter.md`).

## 4. The absolute pointer 21D1:0001 (measured)

HID report descriptor, 45 bytes:

```
05 01 09 02 a1 01 09 01 a1 00 85 63 09 30 09 31 15 00 26 ff 7f 75 10 95 02 81 02
06 00 ff 09 01 15 01 26 ff 00 75 08 95 01 81 02 c0 c0
```

| Item | Meaning |
|---|---|
| Report ID `0x63` | the only report |
| X, Y | 16 bits each, 0..32767, absolute |
| Vendor byte (usage page `0xFF00`) | logical range 1..255 in the descriptor, but the values are 0, 1, 2 ... |
| Buttons | none |

Input report, 6 bytes:

| Byte | Content |
|---|---|
| 0 | `0x63` |
| 1–2 | X, little-endian, 0..32767, **relative to one monitor** |
| 3–4 | Y, little-endian, 0..32767, **relative to one monitor** |
| 5 | **monitor index**, 0-based. The Windows driver uses `index & 0x0F`. |

Rate: one report about every 2 ms while the mouse moves. No reports while the mouse
does not move.

### Monitor index

0..32767 covers one rectangle. For a computer with more than one monitor, the KVM
sends the position relative to one monitor and the index of that monitor. The
computer must convert this to its desktop coordinates:

- **Windows**: the vendor driver does the conversion (see
  `windows-driver-analysis.md`).
- **macOS**: macOS ignores the index byte and maps X/Y onto the display that holds
  the cursor (**measured**: in `data/gscap-capture-2026-10-06.log.gz`, the mapping
  agrees with the display that holds the cursor, not with the index). So the cursor
  stays on one display, and Black Box says that macOS supports only one monitor.
- **Linux**: not tested. X11 maps an absolute pointer onto the full desktop, so the
  index is also necessary there.

### Crossings (measured, personal Mac, 3 displays)

When the internal cursor goes across a screen edge, the index changes. The last report
before the change is near the exit edge, and the first report after it is near the
opposite edge. The coordinate along the edge continues (for example X = 24884 → 25000
at a bottom-to-top crossing). Summary of the first capture:

| From | Exit edge | To | Entry edge | Count |
|---|---|---|---|---|
| 0 | right | 1 | left | 14 |
| 1 | left | 0 | right | 13 |
| 0 | bottom | 2 | top | 8 |
| 2 | top | 0 | bottom | 8 (in one of them, a fast movement put the last report 1559 units from the edge) |

## 5. Keyboard and mouse copies (measured)

| Device | Interfaces | Reports in report protocol |
|---|---|---|
| Launch keyboard `3384:0001` | 1: boot keyboard. 2: vendor `0xFF60` (32-byte raw). 3: report IDs 3 (system control), 4 (consumer control), 5 (NKRO keyboard: modifier byte + 240-bit key bitmap, 5 LED bits out). | Only interface 3 sends key reports. |
| BenQ mouse `04A5:8002` | 4: mouse (5 buttons, X/Y int16, wheel int8). 5: keyboard (macro keys). 6: vendor. | `buttons xL xH yL yH wheel`. X/Y = 0 in glide mode. |

The full descriptors are in `data/pico-kvm-hid-descriptors.log`.

## 6. The layout file (.ffc)

The configuration application saves the layout as an `.ffc` file. Field layout,
decoded from `default.ffc` (all integers little-endian):

```
ff ff ff ff                 header
04 00 00 00                 number of screens
per screen:
  u32  computer (KVM port), 1..4
  u32  monitor index on that computer   → byte 5 of report 0x63
  u32  canvas left, top, right, bottom  (4 values)
  u32  resolution width, height
  u8 + chars  name (only for the application; the switch ignores it)
  ...  more bytes (not decoded; LED assignment is probable)
after the screens: LED colour table ("Red", ...)
```

The computer, index, rectangle and resolution fields agree with the application and
with the captured reports. The other bytes are not decoded.

| Field | Use in the KVM |
|---|---|
| Canvas rectangle | **Topology and encoding.** It defines which edges touch, and where along an edge the cursor enters the next screen (the overlap). It also defines the 0..32767 scale of each screen. A screen that does not touch another screen is ignored. |
| Monitor index | The value in byte 5. The computer must give the same number to the same physical display. |
| Resolution | Probably **movement speed**, not encoding (**inferred**). The help file says that the absolute size of the screens on the canvas "is not important", and that each screen "may" give its resolution. Probable function: the KVM moves its internal cursor in pixels and divides by the resolution. Then the cursor speed is the same on a 1080p screen and a 5K screen. Test: set a wrong resolution for one screen and compare the hand movement necessary to cross it. |

Practical rules:

- The canvas arrangement of each computer must agree with its display arrangement.
  If not, the cursor enters the next display at a different position. Example: on
  the personal Mac, the canvas has the built-in screen centered below the H27P3, but
  macOS has it below the right half. The entry point is then about 1000 pt to the side.
- For a scaled Retina display, the resolution that macOS uses (for example 2560×1600)
  is probably the correct value, not the panel resolution. Not tested.

## 7. Open questions

- Confirm the internal cursor model and the use of the resolution in the firmware
  (`KV0004AR2_V2.10.8665.bin`, not analyzed).
- The remaining bytes of the `.ffc` screen record.
- How the tray application gives the monitor rectangles to the Windows driver
  (probably `DeviceIoControl`, not confirmed).
