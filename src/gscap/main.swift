// gscap: print display layout, then log raw Glide and Switch (21D1:0001) input reports
// with the current cursor position.
// Usage: open -W -n GSCap.app --args [seconds] [logfile]
// The app needs the Input Monitoring permission (System Settings > Privacy & Security).
import Foundation
import IOKit.hid
import CoreGraphics

let args = Array(CommandLine.arguments.dropFirst())
let duration = Double(args.first ?? "60") ?? 60
if args.count > 1 { freopen(args[1], "w", stdout) }
setvbuf(stdout, nil, _IOLBF, 0)
let access = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent)
print("IOHIDCheckAccess=\(access.rawValue) (0=granted 1=denied 2=unknown)")
if access != kIOHIDAccessTypeGranted { print("IOHIDRequestAccess=\(IOHIDRequestAccess(kIOHIDRequestTypeListenEvent))") }

var ids = [CGDirectDisplayID](repeating: 0, count: 16); var n: UInt32 = 0
CGGetActiveDisplayList(16, &ids, &n)
for d in ids.prefix(Int(n)) {
    let b = CGDisplayBounds(d)
    print(String(format: "DISPLAY id=%u main=%d bounds=(%.0f,%.0f %.0fx%.0f) px=%dx%d rot=%.0f vendor=%u model=%u serial=%u",
        d, CGDisplayIsMain(d), b.origin.x, b.origin.y, b.width, b.height,
        CGDisplayPixelsWide(d), CGDisplayPixelsHigh(d), CGDisplayRotation(d),
        CGDisplayVendorNumber(d), CGDisplayModelNumber(d), CGDisplaySerialNumber(d)))
}

let mgr = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
IOHIDManagerSetDeviceMatching(mgr, [kIOHIDVendorIDKey: 0x21D1, kIOHIDProductIDKey: 0x0001] as CFDictionary)
let t0 = Date()
let cb: IOHIDReportCallback = { _, _, _, _, reportID, report, len in
    let bytes = (0..<len).map { String(format: "%02x", report[$0]) }.joined(separator: " ")
    let p = CGEvent(source: nil)?.location ?? .zero
    var x = 0, y = 0, m = -1
    if len >= 6 { x = Int(report[1]) | Int(report[2]) << 8; y = Int(report[3]) | Int(report[4]) << 8; m = Int(report[5]) }
    print(String(format: "%7.3f id=%02x raw=[%@] x=%5d y=%5d mon=%3d cursor=(%.0f,%.0f)",
        Date().timeIntervalSince(t0), reportID, bytes, x, y, m, p.x, p.y))
}
let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: 64)
IOHIDManagerRegisterDeviceMatchingCallback(mgr, { _, _, _, dev in
    let name = IOHIDDeviceGetProperty(dev, kIOHIDProductKey as CFString) as? String ?? "?"
    print("DEVICE matched: \(name)")
}, nil)
IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), CFRunLoopMode.defaultMode.rawValue)
let r = IOHIDManagerOpen(mgr, IOOptionBits(kIOHIDOptionsTypeNone))
print(String(format: "IOHIDManagerOpen=0x%08x", r))
if let devs = IOHIDManagerCopyDevices(mgr) as? Set<IOHIDDevice> {
    for d in devs { IOHIDDeviceRegisterInputReportCallback(d, buf, 64, cb, nil) }
}
print("CAPTURE start, \(duration) s")
CFRunLoopRunInMode(CFRunLoopMode.defaultMode, duration, false)
print("CAPTURE end")
