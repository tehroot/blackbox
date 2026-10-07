// GlideSwitchHelper: multi-monitor Glide and Switch support for macOS.
//
// The Black Box Freedom II (Adder Free-Flow) sends absolute report 0x63 from the
// "Glide and Switch USB Mouse" (21D1:0001):
//   byte 0   report ID 0x63
//   byte 1-2 X, little-endian, 0..32767, relative to ONE monitor
//   byte 3-4 Y, little-endian, 0..32767, relative to ONE monitor
//   byte 5   monitor index (the Windows driver uses index & 0x0F)
// macOS ignores byte 5 and maps X/Y onto the display that holds the cursor.
// This helper seizes the device and puts the cursor on the display that the
// config file assigns to the monitor index.
//
// Usage: GlideSwitchHelper [--dry-run] [--seconds N] [--verbose]
// Config: ~/Library/Application Support/GlideSwitchHelper/config.json
// Log:    ~/Library/Logs/GlideSwitchHelper.log
import Foundation
import IOKit.hid
import CoreGraphics
import ApplicationServices

struct DisplaySel: Codable {
    var index: Int
    var note: String?
    var builtin: Bool?
    var vendor: UInt32?
    var model: UInt32?
    var serial: UInt32?
}
struct Config: Codable { var displays: [DisplaySel] }

// Layout of the first capture on the personal Mac (2026-10-06). Add "serial" to the
// config only to separate two displays of the same model.
let defaultConfig = Config(displays: [
    DisplaySel(index: 0, note: "H27P3", builtin: nil, vendor: 19815, model: 10098, serial: nil),
    DisplaySel(index: 1, note: "LU28R55 portrait", builtin: nil, vendor: 19501, model: 4120, serial: nil),
    DisplaySel(index: 2, note: "Built-in", builtin: true, vendor: nil, model: nil, serial: nil),
])

let args = CommandLine.arguments
let dryRun = args.contains("--dry-run")
let verbose = args.contains("--verbose")
let seconds = args.firstIndex(of: "--seconds").flatMap { i in i + 1 < args.count ? Double(args[i + 1]) : nil }

let fm = FileManager.default
let home = fm.homeDirectoryForCurrentUser
let logURL = home.appendingPathComponent("Library/Logs/GlideSwitchHelper.log")
freopen(logURL.path, "a", stdout)
setvbuf(stdout, nil, _IOLBF, 0)
func log(_ s: String) { print("\(ISO8601DateFormatter().string(from: Date())) \(s)") }

let cfgDir = home.appendingPathComponent("Library/Application Support/GlideSwitchHelper")
let cfgURL = cfgDir.appendingPathComponent("config.json")
func loadConfig() -> Config {
    if let d = try? Data(contentsOf: cfgURL), let c = try? JSONDecoder().decode(Config.self, from: d) { return c }
    try? fm.createDirectory(at: cfgDir, withIntermediateDirectories: true)
    let enc = JSONEncoder(); enc.outputFormatting = [.prettyPrinted, .sortedKeys]
    try? enc.encode(defaultConfig).write(to: cfgURL)
    log("wrote default config \(cfgURL.path)")
    return defaultConfig
}
let config = loadConfig()

// Monitor index -> display bounds in global points. Rebuilt on display changes.
var boundsByIndex: [Int: CGRect] = [:]
func rebuildMap() {
    var ids = [CGDirectDisplayID](repeating: 0, count: 16); var n: UInt32 = 0
    CGGetActiveDisplayList(16, &ids, &n)
    var m: [Int: CGRect] = [:]
    for sel in config.displays {
        let hit = ids.prefix(Int(n)).first { d in
            if let b = sel.builtin, b != (CGDisplayIsBuiltin(d) != 0) { return false }
            if let v = sel.vendor, v != CGDisplayVendorNumber(d) { return false }
            if let v = sel.model, v != CGDisplayModelNumber(d) { return false }
            if let v = sel.serial, v != CGDisplaySerialNumber(d) { return false }
            return true
        }
        if let d = hit {
            m[sel.index] = CGDisplayBounds(d)
            log("index \(sel.index) (\(sel.note ?? "")) -> display \(d) \(CGDisplayBounds(d))")
        } else {
            log("WARNING index \(sel.index) (\(sel.note ?? "")): no matching display")
        }
    }
    boundsByIndex = m
}
rebuildMap()
CGDisplayRegisterReconfigurationCallback({ _, flags, _ in
    if !flags.contains(.beginConfigurationFlag) { rebuildMap() }
}, nil)

// Do not seize without permission to post events: the KVM mouse would stop on this Mac.
let trusted = AXIsProcessTrustedWithOptions([kAXTrustedCheckOptionPrompt.takeUnretainedValue(): true] as CFDictionary)
log("start dryRun=\(dryRun) accessibility=\(trusted) inputMonitoring=\(IOHIDCheckAccess(kIOHIDRequestTypeListenEvent).rawValue) (0=granted)")
if IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) != kIOHIDAccessTypeGranted {
    _ = IOHIDRequestAccess(kIOHIDRequestTypeListenEvent)
}
let seize = !dryRun && trusted
if !dryRun && !trusted { log("Accessibility not granted: device not seized, no events posted") }

let source = CGEventSource(stateID: .hidSystemState)
var last = CGPoint.zero

func handle(x: Int, y: Int, index: Int) {
    let b = boundsByIndex[index] ?? CGDisplayBounds(CGMainDisplayID())
    if boundsByIndex[index] == nil, verbose { log("unmapped index \(index), using main display") }
    let px = min(b.minX + CGFloat(x) * b.width / 32768, b.maxX - 1)
    let py = min(b.minY + CGFloat(y) * b.height / 32768, b.maxY - 1)
    let p = CGPoint(x: px.rounded(.down), y: py.rounded(.down))
    if verbose { log("in x=\(x) y=\(y) index=\(index) -> (\(p.x), \(p.y))") }
    guard seize, p != last else { return }

    // A move with a button held must be a drag event, or drags do not work.
    let left = CGEventSource.buttonState(.combinedSessionState, button: .left)
    let right = CGEventSource.buttonState(.combinedSessionState, button: .right)
    let other = CGEventSource.buttonState(.combinedSessionState, button: .center)
    let (type, button): (CGEventType, CGMouseButton) =
        left ? (.leftMouseDragged, .left) : right ? (.rightMouseDragged, .right) :
        other ? (.otherMouseDragged, .center) : (.mouseMoved, .left)
    if let ev = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: p, mouseButton: button) {
        ev.setIntegerValueField(.mouseEventDeltaX, value: Int64(p.x - last.x))
        ev.setIntegerValueField(.mouseEventDeltaY, value: Int64(p.y - last.y))
        ev.post(tap: .cghidEventTap)
    }
    last = p
}

let mgr = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
IOHIDManagerSetDeviceMatching(mgr, [kIOHIDVendorIDKey: 0x21D1, kIOHIDProductIDKey: 0x0001] as CFDictionary)
let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: 64)
IOHIDManagerRegisterInputReportCallback(mgr, { _, _, _, _, reportID, report, len in
    guard reportID == 0x63, len >= 6 else { return }
    handle(x: Int(report[1]) | Int(report[2]) << 8,
           y: Int(report[3]) | Int(report[4]) << 8,
           index: Int(report[5] & 0x0F))
}, nil)
IOHIDManagerRegisterDeviceMatchingCallback(mgr, { _, r, _, dev in
    log("device matched: \(IOHIDDeviceGetProperty(dev, kIOHIDProductKey as CFString) as? String ?? "?") result=\(r)")
}, nil)
IOHIDManagerRegisterDeviceRemovalCallback(mgr, { _, _, _, _ in log("device removed") }, nil)
IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), CFRunLoopMode.defaultMode.rawValue)
let openResult = IOHIDManagerOpen(mgr, IOOptionBits(seize ? kIOHIDOptionsTypeSeizeDevice : kIOHIDOptionsTypeNone))
log(String(format: "IOHIDManagerOpen seize=%d result=0x%08x", seize ? 1 : 0, openResult))

signal(SIGTERM, SIG_IGN)
let term = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
term.setEventHandler { log("SIGTERM, exit"); exit(0) }
term.resume()

if let s = seconds {
    CFRunLoopRunInMode(CFRunLoopMode.defaultMode, s, false)
    log("run time \(s) s complete, exit")
} else {
    CFRunLoopRun()
}
