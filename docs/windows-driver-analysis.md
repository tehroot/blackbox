# Analysis of the Windows multi-monitor driver

Source: `BlackBox_Multi-Monitor_1.13_beta_July23-2021_27940.zip` →
`Install Glide And Switch Multi-Monitor Driver.exe` (NSIS installer, unpacked with
`7z`). Tools: `tools/re/disasm_pe.py` (Capstone), `tools/re/strings_utf16.py`.

## 1. Package contents

| File | Function |
|---|---|
| `Drivers/HID/mumoHID.inf` | HIDClass. Binds to `USB\VID_21D1&PID_0001`. DriverVer 07/17/2013, 1.1.0.79. KMDF 1.9. |
| `Drivers/HID/mumoHID.sys` | KMDF HID minidriver for the whole USB device. |
| `Drivers/HID/mumoMin.sys` | HID minidriver shim (`HidRegisterMinidriver`), used on Windows XP. Windows 7 and later use `mshidkmdf`. |
| `Drivers/Mouse/mumomou.inf` | Mouse class. Binds to `HID\VID_21D1&PID_0001&Col02`. Includes `HID_Mouse_Absolute_Inst` of `msmouse.inf`. DriverVer 1.2.3.1. |
| `Drivers/Mouse/mumomou.sys` | Mouse class upper filter. |
| `GlideAndSwitch.exe` | MFC tray application: monitor identification and configuration. |
| `default.ffc`, `GlideAndSwitch.chm` | Default layout and help. |

All driver PDB paths start with `c:\winddk\freeflowdriver\driver\source\` (Adder Free-Flow).

Release notes, version 1.13 beta: "Use only on Windows OS, not supported on Linux /
Mac" and "Mac OS will never support more than 1 monitor on Freedom".

## 2. mumoHID.sys: conversion of index to desktop position

The driver gives Windows its own report descriptor (91 bytes, hard-coded):

| Collection | Report ID | Content |
|---|---|---|
| Col01 | `0x01` | relative mouse: 8 buttons, wheel, X/Y 8-bit |
| Col02 | `0x63` | absolute pointer: X/Y 0..0x7FFF (no vendor byte) |

Input processing (function at `0x10bd4`):

1. It accepts a 6-byte packet with report ID `0x63` or `0x01`.
2. For `0x63`, when no monitor table is loaded (`ctx+0x114 == 0`), it passes X/Y
   without change.
3. When a table is loaded: `idx = byte5 & 0x0F` (signed modulo 16). The table entry
   for `idx` is 16 bytes at `ctx + 0x14 + 16·idx`:
   ```
   X_out = ((X · scaleX) >> 15) + offsetX
   Y_out = ((Y · scaleY) >> 15) + offsetY
   ```
4. It sends the result as a 5-byte report `0x63` (X/Y only).

Table calculation (code before `0x106b7`): the input is a list of monitor rectangles
(20-byte entries). The driver finds the bounding box of all monitors, then for each
monitor:

```
offsetX = (left − minLeft) << 15 / totalWidth      scaleX = width  << 15 / totalWidth
offsetY = (top  − minTop)  << 15 / totalHeight     scaleY = height << 15 / totalHeight
```

So the output is a position in the full virtual desktop, normalized to 0..32767.

## 3. mumomou.sys: one flag

The service callback (`0x10486`) goes through each `MOUSE_INPUT_DATA` record (24
bytes). If `Flags & MOUSE_MOVE_ABSOLUTE` (1), it sets `MOUSE_VIRTUAL_DESKTOP` (2). That
is all. Without this flag, Windows maps an absolute position onto the primary monitor
only. With it, Windows maps it onto the full virtual desktop, which is the space that
mumoHID.sys calculates.

## 4. GlideAndSwitch.exe (tray application)

- Imports `EnumDisplayMonitors`, `GetMonitorInfoW`, `MonitorFromPoint`: it reads the
  Windows monitor layout.
- WinINet HTTP (`HttpOpenRequestW`, `HttpSendRequestW`) and the path `/cgi-bin/config`:
  layout transfer to the switch over the network. `\\.\COM%1d` and "RS232 COM1 19200":
  transfer over the serial port.
- Version 1.13 orders the monitors by physical location, because Windows can number
  the monitors again after a power cycle. "Identify Monitors" shows the letters that
  the driver uses.
- How it gives the rectangles to mumoHID.sys is not confirmed (probably
  `DeviceIoControl`).

## 5. Result for other operating systems

The only conversion that macOS and Linux do not have is "monitor index → part of the
desktop". The two macOS solutions in this repository do that conversion:

- `GlideSwitchHelper` does it on the host, the same as mumoHID.sys.
- The Pico adapter does it between the KVM and the host, with a different method,
  because it cannot read the display layout of the host.
