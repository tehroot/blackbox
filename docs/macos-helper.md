# GlideSwitchHelper: multi-monitor Glide and Switch on a Mac with admin access

Use this solution on a Mac where you can grant the Input Monitoring and Accessibility
permissions. For a Mac where you cannot, use the Pico adapter (`pico-adapter.md`).

## 1. Function

1. It opens the KVM absolute pointer (`21D1:0001`) with exclusive access
   (`kIOHIDOptionsTypeSeizeDevice`). macOS then does not move the cursor itself.
2. For each report `0x63`, it finds the display for the monitor index in the config
   file and calculates the position on that display.
3. It sends a mouse-move event (`CGEventPost`). When a button is held, it sends a drag
   event, so drags across displays work.
4. Buttons and wheel come from the copy of the real mouse and go to macOS directly.
   The helper does not touch them.

Safety rule: if the Accessibility permission is missing, the helper does not take
exclusive access. Without this rule, the KVM could not move the cursor at all.

## 2. Files

| Path | Content |
|---|---|
| `src/GlideSwitchHelper/main.swift` | Source |
| `src/GlideSwitchHelper/build.sh` | Builds `GlideSwitchHelper.app` in the repository root (ad-hoc signature) |
| `src/GlideSwitchHelper/install-launchagent.sh` | Installs the LaunchAgent (`--remove` removes it) |
| `src/GlideSwitchHelper/config.example.json` | Config of the personal Mac (3 displays) |
| `~/Library/Application Support/GlideSwitchHelper/config.json` | Active config. The helper writes a default if the file does not exist. |
| `~/Library/Logs/GlideSwitchHelper.log` | Log |
| `~/Library/LaunchAgents/local.glideswitch.helper.plist` | LaunchAgent |

## 3. Install

1. `src/GlideSwitchHelper/build.sh`
2. Start the app one time (`open GlideSwitchHelper.app --args --dry-run --seconds 5`) so
   that macOS lists it.
3. In **System Settings → Privacy & Security**, turn on **GlideSwitchHelper** in
   **Input Monitoring** and in **Accessibility**. If it is not in a list, add it with **+**.
4. `src/GlideSwitchHelper/install-launchagent.sh`
5. Check the log: `IOHIDManagerOpen seize=1 result=0x00000000`.

**Do not move or rename the app after step 4.** The LaunchAgent has its full path.

## 4. Config

Each entry gives a monitor index and the properties that identify a display.
`CGDisplayVendorNumber`, `CGDisplayModelNumber` and `CGDisplaySerialNumber` find
external displays. `builtin: true` finds the built-in display. Vendor and model are
sufficient for different display models. Add `"serial"` only to separate two displays
of the same model.

```json
{
  "displays" : [
    { "index" : 0, "note" : "H27P3",            "vendor" : 19815, "model" : 10098 },
    { "index" : 1, "note" : "LU28R55 portrait", "vendor" : 19501, "model" : 4120 },
    { "index" : 2, "note" : "Built-in",         "builtin" : true }
  ]
}
```

To find the index of each display: run GSCap (`src/gscap/build.sh`), move the cursor
across all displays, and analyze the log with `tools/capture/analyze_capture.py`. The
crossing summary shows which index is past which edge. Compare it with the macOS
display arrangement.

After a config change, restart the helper. You do not need to build it again:
`launchctl kickstart -k gui/$(id -u)/local.glideswitch.helper`

## 5. Command-line options

| Option | Function |
|---|---|
| `--dry-run` | No exclusive access, no events. Only the mapping goes to the log. |
| `--seconds N` | Stop after N seconds (for a safe test) |
| `--verbose` | One log line for each report (large log) |

## 6. Commands

| Task | Command |
|---|---|
| Restart | `launchctl kickstart -k gui/$(id -u)/local.glideswitch.helper` |
| Stop until the next login | `launchctl bootout gui/$(id -u)/local.glideswitch.helper` |
| Remove | `src/GlideSwitchHelper/install-launchagent.sh --remove` |
| Log | `tail ~/Library/Logs/GlideSwitchHelper.log` |
| Stop the helper during a fault | Use the built-in trackpad: `pkill -x GlideSwitchHelper` |

## 7. Limits

- **Ad-hoc signature.** Each build gets a new signature, and macOS cancels the two
  permissions. Grant them again after each build. A self-signed code-signing
  certificate would prevent this (not done).
- **One copy only.** A second copy (for example started from Finder) cannot get
  exclusive access (`0xe00002c5`, `kIOReturnExclusiveAccess`) and does nothing.
- **Login window.** The LaunchAgent starts after login. At the login window, the
  single-display behavior of macOS applies.
- **Edge position.** The KVM calculates where the cursor enters the next display. If
  the KVM layout does not agree with the macOS arrangement, the entry point is to the
  side. Correct the layout in the configuration application.

## 8. Test record

2026-10-06, personal Mac (M2 Pro, macOS 15.7.7, 3 displays): a 90-second run with
17,258 reports (index 0: 11,487, index 1: 2,920, index 2: 2,851). All crossings went to
the correct display. Drags across displays worked. The glide to the other computer
worked.
