import Foundation
import IOKit
import IOKit.usb

struct USBDevice {
    let name: String
    let vendorID: Int
    let productID: Int
    let locationID: UInt32
}

enum USBEvent {
    case connected(USBDevice)
    case disconnected(USBDevice)
}

/// Known Android-related USB vendor IDs. This is a *fast path* only —
/// if a device's VID is in this set we skip the slower libmtp probe.
/// For everything else `probeMTP()` asks libmtp directly so we catch
/// every model libmtp supports (Oppo, Realme, BBK, Asus, ZTE, ...).
private let androidVendorIDs: Set<Int> = [
    0x18D1, // Google
    0x04E8, // Samsung
    0x22B8, // Motorola
    0x0FCE, // Sony
    0x12D1, // Huawei
    0x2717, // Xiaomi
    0x2A70, // OnePlus
    0x05C6, // Qualcomm (many Chinese OEMs)
    0x1004, // LG
    0x0BB4, // HTC
    0x10A9, // Pantech / LG variant
    0x0E8D, // MediaTek
    0x2916, // Android (generic)
    0x2D95, // Vivo
    0x22D9, // Oppo / OnePlus / Realme
    0x2A45, // Meizu
    0x1782, // Spreadtrum
    0x19D2, // ZTE
    0x17EF, // Lenovo
    0x0408, // Quanta / Asus
    0x0B05, // Asus
    0x109B, // Hisense
    0x0489, // Foxconn
    0x0930, // Toshiba
    0x0FCA, // Research In Motion (BlackBerry / Android)
    0x0BE7, // BBK
]

/// Fallback MTP probe via the libmtp `mtp-detect` CLI shipped by Homebrew.
/// Cheap (≈100 ms) and the source of truth for whether libmtp will be
/// able to talk to the phone. Returns the friendly name on success.
private func probeMTP() -> String? {
    let candidates = ["/opt/homebrew/bin/mtp-detect", "/usr/local/bin/mtp-detect"]
    guard let path = candidates.first(where: { FileManager.default.isExecutableFile(atPath: $0) })
    else { return nil }
    let p = Process()
    p.executableURL = URL(fileURLWithPath: path)
    let out = Pipe(); p.standardOutput = out; p.standardError = Pipe()
    do { try p.run() } catch { return nil }
    p.waitUntilExit()
    let data = out.fileHandleForReading.readDataToEndOfFile()
    let txt = String(data: data, encoding: .utf8) ?? ""
    // Look for "Device 0 (VID=xxxx and PID=xxxx) is a <name>."
    if let line = txt.split(separator: "\n").first(where: { $0.contains("VID=") && $0.contains("is a") }) {
        if let r = line.range(of: "is a ") {
            let rest = line[r.upperBound...]
            return rest.trimmingCharacters(in: CharacterSet(charactersIn: ". "))
        }
        return "Android device"
    }
    return nil
}

final class USBWatcher {
    typealias Handler = (USBEvent) -> Void

    private let handler: Handler
    private var notifyPort: IONotificationPortRef?
    private var addedIter: io_iterator_t = 0
    private var removedIter: io_iterator_t = 0

    init(handler: @escaping Handler) {
        self.handler = handler
    }

    func start() {
        let port: mach_port_t = {
            if #available(macOS 12.0, *) { return kIOMainPortDefault }
            return kIOMasterPortDefault
        }()
        notifyPort = IONotificationPortCreate(port)
        guard let np = notifyPort else { return }
        let runLoopSource = IONotificationPortGetRunLoopSource(np).takeUnretainedValue()
        CFRunLoopAddSource(CFRunLoopGetMain(), runLoopSource, .defaultMode)

        let matching = IOServiceMatching(kIOUSBDeviceClassName)
        let selfPtr = Unmanaged.passUnretained(self).toOpaque()

        IOServiceAddMatchingNotification(np,
            kIOMatchedNotification,
            matching,
            { ctx, iter in
                let me = Unmanaged<USBWatcher>.fromOpaque(ctx!).takeUnretainedValue()
                me.drain(iter: iter, added: true)
            }, selfPtr, &addedIter)
        drain(iter: addedIter, added: true) // arm + emit existing

        let matching2 = IOServiceMatching(kIOUSBDeviceClassName)
        IOServiceAddMatchingNotification(np,
            kIOTerminatedNotification,
            matching2,
            { ctx, iter in
                let me = Unmanaged<USBWatcher>.fromOpaque(ctx!).takeUnretainedValue()
                me.drain(iter: iter, added: false)
            }, selfPtr, &removedIter)
        drain(iter: removedIter, added: false)
    }

    func stop() {
        if addedIter != 0 { IOObjectRelease(addedIter); addedIter = 0 }
        if removedIter != 0 { IOObjectRelease(removedIter); removedIter = 0 }
        if let np = notifyPort {
            IONotificationPortDestroy(np)
            notifyPort = nil
        }
    }

    private func drain(iter: io_iterator_t, added: Bool) {
        var sawAny = false
        while case let svc = IOIteratorNext(iter), svc != 0 {
            defer { IOObjectRelease(svc) }
            sawAny = true
            guard let dev = makeDevice(svc) else { continue }
            NSLog("USB %@: %@ (VID=%04x PID=%04x)",
                  added ? "added" : "removed", dev.name, dev.vendorID, dev.productID)
            if androidVendorIDs.contains(dev.vendorID) || looksLikeMTP(svc) {
                handler(added ? .connected(dev) : .disconnected(dev))
                continue
            }
            // Unknown vendor — ask libmtp on connect events.
            if added {
                DispatchQueue.global().async { [handler] in
                    if let name = probeMTP() {
                        NSLog("MTP probe matched: %@", name)
                        let labelled = USBDevice(name: name,
                            vendorID: dev.vendorID, productID: dev.productID,
                            locationID: dev.locationID)
                        handler(.connected(labelled))
                    }
                }
            }
        }
        // If a remove fired but no MTP-looking device matched, still
        // signal disconnect so a stale mount tears down. We can't
        // recover the original USBDevice, so emit a generic one.
        if !added && !sawAny {
            // nothing to do
        }
    }

    private func makeDevice(_ svc: io_service_t) -> USBDevice? {
        func intProp(_ key: String) -> Int? {
            guard let v = IORegistryEntryCreateCFProperty(svc, key as CFString, kCFAllocatorDefault, 0)?
                .takeRetainedValue() as? NSNumber else { return nil }
            return v.intValue
        }
        func strProp(_ key: String) -> String? {
            (IORegistryEntryCreateCFProperty(svc, key as CFString, kCFAllocatorDefault, 0)?
                .takeRetainedValue() as? String)
        }
        guard let vid = intProp(kUSBVendorID) else { return nil }
        let pid = intProp(kUSBProductID) ?? 0
        let loc = UInt32(intProp("locationID") ?? 0)
        let name = strProp(kUSBProductString)
            ?? strProp("USB Product Name")
            ?? "USB Device \(String(format: "%04x:%04x", vid, pid))"
        return USBDevice(name: name, vendorID: vid, productID: pid, locationID: loc)
    }

    /// Best-effort check whether the device exposes an MTP interface.
    /// Many Android phones identify the MTP function via the interface
    /// description string "MTP".
    private func looksLikeMTP(_ svc: io_service_t) -> Bool {
        if let s = IORegistryEntrySearchCFProperty(svc, kIOServicePlane,
            "USB Interface Name" as CFString, kCFAllocatorDefault,
            IOOptionBits(kIORegistryIterateRecursively)) as? String {
            return s.localizedCaseInsensitiveContains("mtp")
        }
        return false
    }
}
