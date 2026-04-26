import Foundation

/// Wired ADB discovery (no wireless / `adb tcpip` in this path).
enum AdbWire {
    private static var adbCandidates: [String] {
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        return [
            "\(home)/Library/Android/sdk/platform-tools/adb",
            "/opt/homebrew/bin/adb",
            "/usr/local/bin/adb",
            "/usr/bin/adb",
        ]
    }

    static func resolvedAdbPath() -> String? {
        let fm = FileManager.default
        if let c = cachedAdbPath { return c }
        for p in adbCandidates where fm.isExecutableFile(atPath: p) {
            cachedAdbPath = p
            return p
        }
        return nil
    }

    private static var cachedAdbPath: String?

    /// Lines like `RFCW123    device usb:1-2 model:Pixel_7 ...`
    private static func deviceLines() -> [String] {
        guard let adb = resolvedAdbPath() else { return [] }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: adb)
        p.arguments = ["devices", "-l"]
        let out = Pipe()
        p.standardOutput = out
        p.standardError = Pipe()
        do { try p.run() } catch { return [] }
        p.waitUntilExit()
        guard p.terminationStatus == 0 else { return [] }
        let data = out.fileHandleForReading.readDataToEndOfFile()
        let txt = String(data: data, encoding: .utf8) ?? ""
        return txt.split(separator: "\n").map(String.init)
    }

    /// Serials in `device` state (USB-attached session).
    static func usbDeviceSerials() -> [String] {
        var serials: [String] = []
        for line in deviceLines() {
            let t = line.trimmingCharacters(in: .whitespaces)
            if t.isEmpty || t.hasPrefix("List of devices") { continue }
            let parts = t.split(whereSeparator: { $0.isWhitespace }).map(String.init)
            guard parts.count >= 2, parts[1] == "device" else { continue }
            let serial = parts[0]
            if serial == "daemon" || serial == "adb" { continue }
            serials.append(serial)
        }
        return serials
    }

    static func ping(serial: String) -> Bool {
        guard let adb = resolvedAdbPath() else { return false }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: adb)
        p.arguments = ["-s", serial, "shell", "echo", "ok"]
        p.standardOutput = Pipe()
        p.standardError = Pipe()
        do { try p.run() } catch { return false }
        p.waitUntilExit()
        return p.terminationStatus == 0
    }

    /// Pick `adb devices` serial for this USB device (single-device fast path + loose `model:` match).
    static func resolveSerial(for device: USBDevice) -> String? {
        let serials = usbDeviceSerials()
        if serials.count == 1, ping(serial: serials[0]) {
            return serials[0]
        }
        let needle = normalizeName(device.name)
        if needle.count < 3 { return nil }
        for line in deviceLines() {
            let t = line.trimmingCharacters(in: .whitespaces)
            guard let r = line.range(of: "model:") else { continue }
            let after = line[r.upperBound...]
            let mod = after.split(separator: " ").first.map(String.init) ?? ""
            let mnorm = normalizeName(mod)
            if mnorm.count >= 3 && (mnorm.contains(needle) || needle.contains(mnorm)) {
                let parts = t.split(whereSeparator: { $0.isWhitespace }).map(String.init)
                guard let serial = parts.first, parts.count >= 2, parts[1] == "device" else { continue }
                if ping(serial: serial) { return serial }
            }
        }
        return nil
    }

    private static func normalizeName(_ s: String) -> String {
        let lower = s.lowercased()
        return lower.filter { $0.isLetter || $0.isNumber }
    }
}
