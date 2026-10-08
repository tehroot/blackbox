# Glide and Switch multi-monitor for macOS

The Black Box ServSwitch Freedom II (KV0004A-R2) moves the mouse and keyboard between
computers when the cursor crosses a screen edge ("Glide and Switch"). On a computer
with more than one monitor, this works only on Windows, with a vendor driver. Black Box
says that macOS supports only one monitor.

This repository has two solutions for macOS, and the analysis behind them.

| Solution | For | Software on the Mac | Status (2026-10-06) |
|---|---|---|---|
| **GlideSwitchHelper** | a Mac with admin access | a small app, Input Monitoring and Accessibility permissions | works (personal Mac, 3 displays) |
| **Pico adapter** | a locked-down Mac | none | works (work Mac, 2 displays; personal Mac, 3 displays) |

## Cause, in short

The KVM gives each computer an absolute pointer (USB `21D1:0001`). Each report has X/Y
(0..32767) **relative to one monitor** and a **monitor index**. The Windows driver
converts index + X/Y to a desktop position. macOS ignores the index and maps X/Y onto
the display that holds the cursor, so the cursor cannot leave that display.

- The helper does the conversion on the Mac (index → display → `CGEventPost`).
- The Pico sits between the KVM and the Mac. It sends absolute X/Y while the index stays
  the same. When the index changes, it sends a relative "push" across the exit edge, so
  macOS moves the cursor onto the next display. It learns the edges from the KVM's
  crossings and keeps them in its flash. It also keeps an event log in its flash, which
  you read on another Mac (`tools/pico-log.sh`).

## Repository

```
README.md                         this file
docs/
  kvm-architecture.md             how the KVM works: USB structure, report format, .ffc layout file
  windows-driver-analysis.md      what the Windows driver does (disassembly results)
  macos-helper.md                 GlideSwitchHelper: install, config, limits
  pico-adapter.md                 Pico adapter: wiring, build, flash, LED, design, fault history
  vendor-files.md                 the Black Box files (not in git)
src/
  GlideSwitchHelper/              macOS helper (Swift), build and LaunchAgent scripts
  gscap/                          HID capture tool (Swift): raw KVM reports + cursor position
  glide-pico/                     Pico firmware (C, pico-sdk + TinyUSB + Pico-PIO-USB)
tools/
  setup-pico-toolchain.sh         installs the Pico toolchain and picotool into ~/pico (no admin)
  pico-log.sh                     reads the Pico's flash log (Pico in BOOTSEL mode)
  pico-log-decode.py              converts a flash log dump to text
  capture/analyze_capture.py      analyzes a GSCap log: index segments and crossings
  re/disasm_pe.py                 disassembles the 32-bit Windows driver files
  re/strings_utf16.py             finds wide strings in Windows binaries
data/
  gscap-capture-2026-10-06.log.gz first capture on the personal Mac (3 displays)
  pico-kvm-hid-descriptors.log    all KVM HID descriptors, read through the Pico
  pico-adapter-0.2-crossings.log  Pico log of a test with 22 crossings
  pico-adapter-0.5-flash-log.txt  Pico flash log, boots 1-5 (versions 0.5 and 0.5.1; faults 8 and 9)
```

Not in git (see `.gitignore`): build output (`*.uf2`, `*.app`, `src/glide-pico/build/`),
Pico logs (`logs/`), and the Black Box vendor files.

## Quick start

**Helper** (Mac with admin access):

```sh
src/GlideSwitchHelper/build.sh
open GlideSwitchHelper.app --args --dry-run --seconds 5   # registers the app with macOS
# System Settings → Privacy & Security: turn on Input Monitoring and Accessibility
src/GlideSwitchHelper/install-launchagent.sh
```

Then set the display of each monitor index in
`~/Library/Application Support/GlideSwitchHelper/config.json`. See `docs/macos-helper.md`.

**Pico adapter:**

```sh
tools/setup-pico-toolchain.sh
src/glide-pico/build.sh
# hold BOOTSEL, connect the Pico, copy glide_pico_work.uf2 to RPI-RP2
```

Wiring and use: see `docs/pico-adapter.md`.

**KVM layout:** in the Glide and Switch configuration application, give each computer
one screen for each monitor, in the same arrangement as on that computer. Send the
layout to the switch.

## Safety and limits

- The helper takes exclusive access to the KVM pointer. If the cursor does not move
  correctly, use the built-in trackpad and run `pkill -x GlideSwitchHelper`.
- The Pico firmware uses the development USB vendor ID `0xCAFE`. Do not distribute
  devices with it.
- The work build of the Pico has no serial port. A new flash needs BOOTSEL.
