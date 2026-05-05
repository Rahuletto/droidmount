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

final class USBWatcher {
    typealias Handler = (USBEvent) -> Void

    private let handler: Handler
    private let probeStateLock = NSLock()
    private var publishedConnectedLocations: Set<UInt32> = []

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
                guard let ctx = ctx else { return }
                let me = Unmanaged<USBWatcher>.fromOpaque(ctx).takeUnretainedValue()
                me.drain(iter: iter, added: true)
            }, selfPtr, &addedIter)
        drain(iter: addedIter, added: true) // arm + emit existing

        let matching2 = IOServiceMatching(kIOUSBDeviceClassName)
        IOServiceAddMatchingNotification(np,
            kIOTerminatedNotification,
            matching2,
            { ctx, iter in
                guard let ctx = ctx else { return }
                let me = Unmanaged<USBWatcher>.fromOpaque(ctx).takeUnretainedValue()
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
        while case let svc = IOIteratorNext(iter), svc != 0 {
            defer { IOObjectRelease(svc) }
            guard let dev = makeDevice(svc) else { continue }
            NSLog("USB %@: %@ (VID=%04x PID=%04x)",
                  added ? "added" : "removed", dev.name, dev.vendorID, dev.productID)

            if !added {
                clearPublishedConnected(dev.locationID)
                handler(.disconnected(dev))
                continue
            }

            /* Do not run mtp-detect here: it races mtpfuse for libusb interface claim.
             * Just publish USB connect once; mount path determines MTP readiness. */
            _ = looksLikeMTP(svc) /* keep for future heuristics + side effects-free introspection */
            emitConnectedIfNeeded(dev)
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
        let product = strProp(kUSBProductString)
            ?? strProp("USB Product Name")
        let serial = strProp(kUSBSerialNumberString)
            ?? strProp("USB Serial Number")
        let name = extractModelCode(from: [product, serial].compactMap { $0 })
            ?? product
            ?? "USB Device \(String(format: "%04x:%04x", vid, pid))"
        return USBDevice(name: name, vendorID: vid, productID: pid, locationID: loc)
    }

    private func extractModelCode(from candidates: [String]) -> String? {
        let prefixes = ["CPH", "RMX", "SM-", "V", "M", "IN", "NE", "LE", "PJD"]
        for src in candidates {
            let toks = src
                .split { !$0.isLetter && !$0.isNumber && $0 != "-" && $0 != "_" }
                .map(String.init)
            for tok in toks {
                let up = tok.uppercased()
                if up.count < 4 { continue }
                if prefixes.contains(where: { up.hasPrefix($0) }) {
                    return tok
                }
            }
        }
        return nil
    }

    /// Best-effort check whether the device exposes an MTP interface.
    /// Currently not a hard gate; kept for diagnostics/future routing.
    private func looksLikeMTP(_ svc: io_service_t) -> Bool {
        if let s = IORegistryEntrySearchCFProperty(svc, kIOServicePlane,
            "USB Interface Name" as CFString, kCFAllocatorDefault,
            IOOptionBits(kIORegistryIterateRecursively)) as? String {
            return s.localizedCaseInsensitiveContains("mtp")
        }
        return false
    }

    private func emitConnectedIfNeeded(_ dev: USBDevice) {
        probeStateLock.lock()
        let inserted = publishedConnectedLocations.insert(dev.locationID).inserted
        probeStateLock.unlock()
        if inserted {
            handler(.connected(dev))
        }
    }

    private func clearPublishedConnected(_ locationID: UInt32) {
        probeStateLock.lock()
        publishedConnectedLocations.remove(locationID)
        probeStateLock.unlock()
    }
}
