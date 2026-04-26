import AppKit
import Darwin
import Foundation

/// One macFUSE + mtpfuse process per USB location (supports several phones at once).
final class MountManager {
    /// How the mount is backed; drives Finder / Disk Utility volume title via FUSE `volname=`.
    enum MountTransport: Equatable {
        case mtp
        /// `mtpfuse` with **MTP_HYBRID_ADB**: same folders as MTP; file reads use ADB when mapped.
        /// `mtpfuse` with **MTP_HYBRID_ADB**: MTP **folders**; reads/writes via **ADB**. Finder title `… (ADB)`.
        case mtpHybrid
        /// Wired `adbfuse` (standalone ADB FUSE — not used for menu “Request ADB mode”).
        case adb
    }

    enum MountError: LocalizedError {
        case helperMissing
        case mountFailed(String)
        case mountpointBusy
        var errorDescription: String? {
            switch self {
            case .helperMissing:
                return "FUSE helper (mtpfuse / adbfuse) not found. Run `make bundle` from the project."
            case .mountFailed(let s):
                return "Mount failed: \(s)"
            case .mountpointBusy:
                return "Mount point is already in use."
            }
        }
    }

    private struct Session {
        let mountPoint: String
        /// Nil when the session was **adopted** after relaunch (FUSE still up; we didn’t spawn this `Process`).
        let process: Process?
        let transport: MountTransport
    }

    private let parentDir: String = "\(NSHomeDirectory())/.AndroidMount"

    /// Optional `volicon=` for macFUSE (Finder often still shows a generic disk for user FUSE).
    private static let volumeIconCandidates: [String] = [
        "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/SidebariPhone.icns",
        "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/com.apple.iphone.icns",
    ]

    private var sessions: [UInt32: Session] = [:]
    private let sessionsLock = NSLock()
    private var cachedMtpfusePath: String?
    private var cachedAdbfusePath: String?
    private static var cachedVolIconPath: String?

    var hasActiveMountSessions: Bool {
        sessionsLock.lock()
        defer { sessionsLock.unlock() }
        return !sessions.isEmpty
    }

    /// If the app restarts while `mtpfuse` is still mounted, USB state is empty but `~/.AndroidMount/<name>_<locHex>`
    /// still exists. Recover menu + eject by parsing the location id from the folder name.
    func adoptOrphanMountsIfNeeded() -> [(locationID: UInt32, displayName: String)] {
        let fm = FileManager.default
        guard let entries = try? fm.contentsOfDirectory(atPath: parentDir) else { return [] }
        var out: [(locationID: UInt32, displayName: String)] = []
        sessionsLock.lock()
        defer { sessionsLock.unlock() }
        for name in entries {
            let path = "\(parentDir)/\(name)"
            var isDir: ObjCBool = false
            guard fm.fileExists(atPath: path, isDirectory: &isDir), isDir.boolValue else { continue }
            guard isFuseMounted(at: path) else { continue }
            if !verifyBrowsableVolume(at: path, transport: .mtp, quick: true) {
                let stale = Session(mountPoint: path, process: nil, transport: .mtp)
                DispatchQueue.global(qos: .utility).async { [weak self] in
                    self?.tearDown(session: stale)
                }
                continue
            }
            let parts = name.split(separator: "_", omittingEmptySubsequences: false)
            guard parts.count >= 2,
                  let tail = parts.last,
                  tail.count == 8,
                  let lid = UInt32(tail, radix: 16) else { continue }
            if sessions[lid] != nil { continue }
            let baseParts = parts.dropLast()
            let display = baseParts.isEmpty ? name : baseParts.joined(separator: "_")
            sessions[lid] = Session(mountPoint: path, process: nil, transport: .mtp)
            Self.applyFinderVolumeIcon(at: path)
            out.append((locationID: lid, displayName: display))
        }
        return out
    }

    /// Characters safe inside macFUSE `volname=` (commas would split `-o` tokens).
    private static func sanitizeVolumeLabel(_ name: String) -> String {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        let mapped = trimmed.map { c -> Character in
            if c == "," { return "_" }
            if c.isLetter || c.isNumber || c == "_" || c == "-" || c == " " || c == "(" || c == ")" {
                return c
            }
            return "_"
        }
        let s = String(mapped)
        let collapsed = s.replacingOccurrences(of: "__", with: "_")
        let limited = String(collapsed.prefix(64))
        return limited.trimmingCharacters(in: CharacterSet(charactersIn: "_")).isEmpty ? "Device" : limited
    }

    /// FUSE `volname=` — **(ADB)** for hybrid or `adbfuse`; plain MTP uses the device name only.
    private static func fuseVolumeLabel(deviceName: String, transport: MountTransport) -> String {
        let trimmed = deviceName.trimmingCharacters(in: .whitespacesAndNewlines)
        let raw: String
        switch transport {
        case .mtp:
            raw = trimmed
        case .mtpHybrid, .adb:
            raw = trimmed.isEmpty ? "(ADB)" : "\(trimmed) (ADB)"
        }
        return sanitizeVolumeLabel(raw)
    }

    /// Outcome of **Request ADB mode** (menu): **mtpfuse** ADB mode when `adb` works, else plain MTP.
    struct AdbModeRequestResult {
        let mountPath: String
        let usingAdb: Bool
        /// Shown when the volume is usable on MTP but hybrid ADB was not enabled (restore or already tried).
        let mtpFallbackMessage: String?
    }

    /// USB connect and menu **Mount** use MTP. **Request ADB mode** remounts with MTP folders + ADB I/O (title `… (ADB)`).
    func mount(device: USBDevice, completion: @escaping (Result<String, Error>) -> Void) {
        startFuseMount(device: device, transport: .mtp, adbSerial: nil, completion: completion)
    }

    /// Unmount then remount **mtpfuse** in ADB mode (MTP `readdir`, ADB reads/writes; Finder shows `… (ADB)`).
    func requestAdbMode(device: USBDevice, completion: @escaping (Result<AdbModeRequestResult, Error>) -> Void) {
        sessionsLock.lock()
        let snap = sessions[device.locationID]
        sessionsLock.unlock()
        guard let cur = snap else {
            completion(.failure(MountError.mountFailed("No active mount for this device.")))
            return
        }
        if cur.transport == .mtpHybrid {
            completion(.success(AdbModeRequestResult(
                mountPath: cur.mountPoint, usingAdb: true, mtpFallbackMessage: nil)))
            return
        }
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            self.unmountBlocking(locationID: device.locationID)
            Thread.sleep(forTimeInterval: 0.6)
            let finish: (Result<AdbModeRequestResult, Error>) -> Void = { r in
                DispatchQueue.main.async { completion(r) }
            }
            guard let serial = AdbWire.resolveSerial(for: device), AdbWire.ping(serial: serial) else {
                self.startFuseMount(device: device, transport: .mtp, adbSerial: nil) { mtp in
                    switch mtp {
                    case .success(let path):
                        finish(.success(AdbModeRequestResult(
                            mountPath: path,
                            usingAdb: false,
                            mtpFallbackMessage:
                                "Could not reach this device over ADB. Unlock the phone, enable USB debugging, "
                                + "authorize this Mac, and ensure `adb` is installed. Restored MTP mount.")))
                    case .failure(let e):
                        finish(.failure(e))
                    }
                }
                return
            }
            self.startFuseMount(device: device, transport: .mtpHybrid, adbSerial: serial) { hy in
                switch hy {
                case .success(let path):
                    finish(.success(AdbModeRequestResult(
                        mountPath: path, usingAdb: true, mtpFallbackMessage: nil)))
                case .failure(let hyErr):
                    let detail = (hyErr as? LocalizedError)?.errorDescription ?? hyErr.localizedDescription
                    self.startFuseMount(device: device, transport: .mtp, adbSerial: nil) { mtp in
                        switch mtp {
                        case .success(let path):
                            finish(.success(AdbModeRequestResult(
                                mountPath: path,
                                usingAdb: false,
                                mtpFallbackMessage:
                                    "Could not enable ADB mode. Restored MTP mount. \(detail)")))
                        case .failure(let e):
                            finish(.failure(e))
                        }
                    }
                }
            }
        }
    }

    private func unmountBlocking(locationID: UInt32) {
        sessionsLock.lock()
        guard let s = sessions.removeValue(forKey: locationID) else {
            sessionsLock.unlock()
            return
        }
        sessionsLock.unlock()
        tearDown(session: s)
    }

    func transport(for locationID: UInt32) -> MountTransport? {
        sessionsLock.lock()
        defer { sessionsLock.unlock() }
        return sessions[locationID]?.transport
    }

    private func startFuseMount(
        device: USBDevice,
        transport: MountTransport,
        adbSerial: String?,
        completion: @escaping (Result<String, Error>) -> Void
    ) {
        sessionsLock.lock()
        if let existing = sessions[device.locationID] {
            sessionsLock.unlock()
            completion(.success(existing.mountPoint))
            return
        }
        sessionsLock.unlock()

        let helper: String?
        switch transport {
        case .adb:
            helper = resolvedAdbfusePath()
        case .mtp, .mtpHybrid:
            helper = resolvedMtpfusePath()
        }
        guard let helperPath = helper else {
            completion(.failure(MountError.helperMissing))
            return
        }

        let safeDir = device.name
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "..", with: "_")
            .trimmingCharacters(in: .whitespaces)
        let tag = String(format: "%08x", device.locationID)
        let base = safeDir.isEmpty ? "Device" : safeDir
        let mountPoint = "\(parentDir)/\(base)_\(tag)"

        do {
            try ensureMountpoint(at: mountPoint)
        } catch {
            completion(.failure(error))
            return
        }

        let safeVol = Self.fuseVolumeLabel(deviceName: device.name, transport: transport)
        let fuseOpts =
            "local,defer_permissions,noappledouble,noatime," +
            "iosize=1048576,daemon_timeout=300," +
            "attr_timeout=3600,entry_timeout=3600,negative_timeout=3600" +
            ",volname=\(safeVol)"

        var argv: [String] = ["-f", "-o", fuseOpts]
        if let v = Self.resolvedVolumeIconPath() {
            argv.append("-o")
            argv.append("volicon=\(v)")
        }

        let p = Process()
        p.executableURL = URL(fileURLWithPath: helperPath)
        p.arguments = argv + [mountPoint]

        var env = ProcessInfo.processInfo.environment
        env["DYLD_FALLBACK_LIBRARY_PATH"] =
            "/opt/homebrew/lib:/usr/local/lib:/Library/Frameworks/macFUSE.framework/Versions/A:" +
            (env["DYLD_FALLBACK_LIBRARY_PATH"] ?? "")
        switch transport {
        case .mtp:
            env["MTP_USB_BUS_LOCATION"] = String(device.locationID)
            env.removeValue(forKey: "ADB_SERIAL")
            env.removeValue(forKey: "ADB_DEVICE_PREFIX")
            env.removeValue(forKey: "MTP_HYBRID_ADB")
        case .mtpHybrid:
            env["MTP_USB_BUS_LOCATION"] = String(device.locationID)
            env["MTP_HYBRID_ADB"] = "1"
            if env["ANDROIDMOUNT_ADB_MAX_PARALLEL"] == nil {
                env["ANDROIDMOUNT_ADB_MAX_PARALLEL"] = "5"
            }
            if let s = adbSerial, !s.isEmpty {
                env["ADB_SERIAL"] = s
            }
            env.removeValue(forKey: "ADB_DEVICE_PREFIX")
        case .adb:
            env.removeValue(forKey: "MTP_USB_BUS_LOCATION")
            env.removeValue(forKey: "MTP_HYBRID_ADB")
            if let s = adbSerial, !s.isEmpty {
                env["ADB_SERIAL"] = s
            }
            env["ADB_DEVICE_PREFIX"] = "/storage"
        }
        if let ic = Self.resolvedDriveIcnsPath() {
            env["MTP_VOLUME_ICON_PATH"] = ic
        }
        p.environment = env

        let errPipe = Pipe()
        p.standardError = errPipe
        p.standardOutput = Pipe()

        let stderrLock = NSLock()
        var stderrBuf = Data()
        let stderrMax = 64 * 1024
        errPipe.fileHandleForReading.readabilityHandler = { handle in
            let chunk = handle.availableData
            if chunk.isEmpty { return }
            stderrLock.lock()
            if stderrBuf.count < stderrMax {
                let room = stderrMax - stderrBuf.count
                stderrBuf.append(chunk.prefix(room))
            }
            stderrLock.unlock()
        }

        let helperTag = transport == .adb ? "adbfuse" : "mtpfuse"
        var didReturn = false
        let returnOnce: (Result<String, Error>) -> Void = { r in
            if !didReturn { didReturn = true; completion(r) }
        }

        func drainStderrText() -> String {
            stderrLock.lock()
            let snap = stderrBuf
            stderrLock.unlock()
            let t = String(data: snap, encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            return t
        }

        p.terminationHandler = { proc in
            errPipe.fileHandleForReading.readabilityHandler = nil
            let remainder = errPipe.fileHandleForReading.readDataToEndOfFile()
            if !remainder.isEmpty {
                stderrLock.lock()
                if stderrBuf.count < stderrMax {
                    let room = stderrMax - stderrBuf.count
                    stderrBuf.append(remainder.prefix(room))
                }
                stderrLock.unlock()
            }
            if !didReturn {
                let fromPipe = drainStderrText()
                let code = proc.terminationStatus
                let msg: String
                if fromPipe.isEmpty {
                    msg =
                        "process exited with status \(code) before the volume appeared (no stderr). " +
                        (transport == .adb
                            ? "Check USB debugging is on and this Mac is authorized."
                            : "Is the phone in File transfer (MTP) mode and unlocked?")
                } else {
                    msg = fromPipe
                }
                returnOnce(.failure(MountError.mountFailed(msg)))
            }
        }

        do {
            try p.run()
            DispatchQueue.global().async { [weak self] in
                guard let self = self else { return }
                for _ in 0..<40 {
                    Thread.sleep(forTimeInterval: 0.2)
                    if self.isFuseMounted(at: mountPoint) {
                        if !self.verifyBrowsableVolume(at: mountPoint, transport: transport) {
                            let msg: String
                            if transport == .adb {
                                msg =
                                    "ADB mount came up empty — unlock the phone, confirm USB debugging, " +
                                    "or try unplugging and reconnecting."
                            } else {
                                msg =
                                    "No storage is visible yet — unlock the phone and set USB to File transfer (MTP). " +
                                    "If it already is, try unplugging and reconnecting."
                            }
                            returnOnce(.failure(MountError.mountFailed(msg)))
                            self.abortEphemeralFuseMount(process: p, mountPoint: mountPoint)
                            return
                        }
                        self.sessionsLock.lock()
                        self.sessions[device.locationID] =
                            Session(mountPoint: mountPoint, process: p, transport: transport)
                        self.sessionsLock.unlock()
                        Self.scheduleSpotlightExcluded(for: mountPoint)
                        Self.applyFinderVolumeIcon(at: mountPoint)
                        returnOnce(.success(mountPoint))
                        return
                    }
                    if !p.isRunning { break }
                }
                if p.isRunning {
                    p.terminate()
                    DispatchQueue.global().asyncAfter(deadline: .now() + 1.0) {
                        if p.isRunning { kill(p.processIdentifier, SIGKILL) }
                    }
                }
                let errTail = drainStderrText()
                var detail = "timed out waiting for FUSE mount (~8s)."
                if !errTail.isEmpty {
                    detail += " \(helperTag) said:\n" + errTail
                } else if !p.isRunning {
                    detail +=
                        transport == .adb
                        ? " \(helperTag) exited; check USB debugging and `adb devices`."
                        : " \(helperTag) exited; check MTP mode and USB connection."
                }
                returnOnce(.failure(MountError.mountFailed(detail)))
            }
        } catch {
            completion(.failure(error))
        }
    }

    func unmount(locationID: UInt32? = nil) {
        sessionsLock.lock()
        let targets: [Session]
        if let id = locationID {
            if let s = sessions.removeValue(forKey: id) {
                targets = [s]
            } else {
                targets = []
            }
        } else {
            targets = Array(sessions.values)
            sessions.removeAll()
        }
        sessionsLock.unlock()

        guard !targets.isEmpty else { return }
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            for s in targets { self.tearDown(session: s) }
        }
    }

    func unmountBlockingForQuit() {
        sessionsLock.lock()
        let targets = Array(sessions.values)
        sessions.removeAll()
        sessionsLock.unlock()
        for s in targets { tearDown(session: s) }
    }

    @discardableResult
    func reconcileStaleSessions() -> Bool {
        sessionsLock.lock()
        let snapshot = sessions
        var removed: [Session] = []
        var dropKeys: [UInt32] = []
        for (loc, s) in snapshot {
            if !isFuseMounted(at: s.mountPoint) {
                dropKeys.append(loc)
                removed.append(s)
            }
        }
        for k in dropKeys { sessions.removeValue(forKey: k) }
        sessionsLock.unlock()
        guard !removed.isEmpty else { return false }
        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let self = self else { return }
            for s in removed { self.tearDown(session: s) }
        }
        return true
    }

    private func tearDown(session: Session) {
        let mp = session.mountPoint

        if let p = session.process {
            if p.isRunning {
                p.terminate()
            }
            var waited = 0
            while p.isRunning && waited < 80 {
                Thread.sleep(forTimeInterval: 0.05)
                waited += 1
            }
            if p.isRunning {
                kill(p.processIdentifier, SIGKILL)
                Thread.sleep(forTimeInterval: 0.15)
            }
        }

        terminateFuseHelpers(mountPoint: mp)

        runDiskutilUnmountForce(mp)
        Thread.sleep(forTimeInterval: 0.15)
        runDiskutilUnmountForce(mp)

        DispatchQueue.global().asyncAfter(deadline: .now() + 0.4) { [weak self] in
            guard let self = self else { return }
            if !self.isFuseMounted(at: mp) {
                try? FileManager.default.removeItem(atPath: mp)
            }
        }
    }

    func mountPoint(for locationID: UInt32) -> String? {
        sessionsLock.lock()
        defer { sessionsLock.unlock() }
        return sessions[locationID]?.mountPoint
    }

    /// Wait until the FUSE root shows real content (MTP storage or `/storage` children over ADB).
    /// - Parameter quick: Shorter polling when adopting orphans at launch (avoid blocking UI).
    private func verifyBrowsableVolume(at mountPoint: String, transport: MountTransport, quick: Bool = false) -> Bool {
        let fm = FileManager.default
        let ignored = Set([
            ".metadata_never_index", ".DS_Store",
        ])
        let attempts: Int
        let step: TimeInterval
        switch transport {
        case .adb:
            attempts = quick ? 12 : 40
            step = quick ? 0.08 : 0.15
        case .mtp, .mtpHybrid:
            attempts = quick ? 18 : 70
            step = quick ? 0.1 : 0.2
        }
        for i in 0..<attempts {
            if i > 0 {
                Thread.sleep(forTimeInterval: step)
            }
            guard let names = try? fm.contentsOfDirectory(atPath: mountPoint) else { continue }
            var meaningfulDirs = 0
            var meaningfulFiles = 0
            for name in names {
                if name.hasPrefix("._") { continue }
                if ignored.contains(name) { continue }
                let full = (mountPoint as NSString).appendingPathComponent(name)
                var isDir: ObjCBool = false
                guard fm.fileExists(atPath: full, isDirectory: &isDir) else { continue }
                if isDir.boolValue {
                    meaningfulDirs += 1
                } else {
                    meaningfulFiles += 1
                }
            }
            if meaningfulDirs >= 1 { return true }
            if meaningfulFiles >= 1 { return true }
        }
        return false
    }

    private func abortEphemeralFuseMount(process: Process, mountPoint: String) {
        if process.isRunning {
            process.terminate()
        }
        for _ in 0..<80 {
            Thread.sleep(forTimeInterval: 0.05)
            if !isFuseMounted(at: mountPoint), !process.isRunning { break }
        }
        if process.isRunning {
            kill(process.processIdentifier, SIGKILL)
        }
        Thread.sleep(forTimeInterval: 0.12)
        if isFuseMounted(at: mountPoint) {
            terminateFuseHelpers(mountPoint: mountPoint)
            runDiskutilUnmountForce(mountPoint)
            runDiskutilUnmountForce(mountPoint)
        }
    }

    private func isFuseMounted(at path: String) -> Bool {
        var st = statfs()
        guard statfs(path, &st) == 0 else { return false }
        let fstype = withUnsafeBytes(of: &st.f_fstypename) { raw -> String in
            guard let base = raw.baseAddress else { return "" }
            return String(cString: base.assumingMemoryBound(to: CChar.self))
        }
        return fstype.contains("fuse") || fstype.contains("macfuse")
    }

    private func ensureMountpoint(at mountPoint: String) throws {
        let fm = FileManager.default
        if !fm.fileExists(atPath: parentDir) {
            try fm.createDirectory(atPath: parentDir,
                withIntermediateDirectories: true, attributes: nil)
        }
        if !fm.fileExists(atPath: mountPoint) {
            try fm.createDirectory(atPath: mountPoint,
                withIntermediateDirectories: true, attributes: nil)
            return
        }
        if isFuseMounted(at: mountPoint) {
            reclaimStaleFuseMount(at: mountPoint)
            if isFuseMounted(at: mountPoint) {
                throw MountError.mountpointBusy
            }
        }
    }

    private func reclaimStaleFuseMount(at path: String) {
        runDiskutilUnmountForce(path)
        terminateFuseHelpers(mountPoint: path)
        runDiskutilUnmountForce(path)
    }

    private static func scheduleSpotlightExcluded(for mountPoint: String) {
        DispatchQueue.global().asyncAfter(deadline: .now() + 0.35) {
            let noIndex = "\(mountPoint)/.metadata_never_index"
            _ = FileManager.default.createFile(atPath: noIndex, contents: nil)
            let task = Process()
            task.executableURL = URL(fileURLWithPath: "/usr/bin/xattr")
            task.arguments = ["-w", "-s", "com.apple.metadata.spotlight.indexing-disable", "1", mountPoint]
            try? task.run()
            task.waitUntilExit()
        }
    }

    /// `icon/drive.icns` copied to `Contents/Resources/drive.icns` by `make bundle`, or next to `build/` in dev.
    private static func resolvedDriveIcnsPath() -> String? {
        let fm = FileManager.default
        if let p = Bundle.main.path(forResource: "drive", ofType: "icns"), fm.fileExists(atPath: p) {
            return p
        }
        if let exe = Bundle.main.executableURL {
            let candidate = (exe.deletingLastPathComponent().appendingPathComponent("../icon/drive.icns")).path
            let std = (candidate as NSString).standardizingPath
            if fm.fileExists(atPath: std) { return std }
        }
        return nil
    }

    private static func resolvedVolumeIconPath() -> String? {
        if let c = cachedVolIconPath { return c }
        let fm = FileManager.default
        var paths: [String] = []
        if let p = resolvedDriveIcnsPath() {
            paths.append(p)
        }
        paths.append(contentsOf: volumeIconCandidates.filter { fm.fileExists(atPath: $0) })
        let v = paths.first
        cachedVolIconPath = v
        return v
    }

    /// Finder often ignores FUSE `volicon=`; set the mount folder icon from `drive.icns` when available.
    private static func applyFinderVolumeIcon(at mountPoint: String) {
        guard let iconPath = resolvedDriveIcnsPath(),
              let image = NSImage(contentsOfFile: iconPath) else { return }
        DispatchQueue.main.async {
            NSWorkspace.shared.setIcon(image, forFile: mountPoint, options: [])
        }
    }

    private func terminateFuseHelpers(mountPoint: String) {
        for pid in fuseHelperPids(usingMountPoint: mountPoint) {
            kill(pid, SIGTERM)
        }
        Thread.sleep(forTimeInterval: 0.35)
        for pid in fuseHelperPids(usingMountPoint: mountPoint) {
            kill(pid, SIGKILL)
        }
        Thread.sleep(forTimeInterval: 0.2)
    }

    private func runDiskutilUnmountForce(_ path: String) {
        let du = Process()
        du.executableURL = URL(fileURLWithPath: "/usr/sbin/diskutil")
        du.arguments = ["unmount", "force", path]
        du.standardOutput = Pipe(); du.standardError = Pipe()
        try? du.run(); du.waitUntilExit()
    }

    private func fuseHelperPids(usingMountPoint path: String) -> [pid_t] {
        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/bin/ps")
        task.arguments = ["-axo", "pid=,command="]
        let out = Pipe()
        task.standardOutput = out
        task.standardError = Pipe()
        do { try task.run() } catch { return [] }
        task.waitUntilExit()
        let data = out.fileHandleForReading.readDataToEndOfFile()
        guard let text = String(data: data, encoding: .utf8) else { return [] }
        var pids: [pid_t] = []
        for raw in text.split(separator: "\n") {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty { continue }
            let parts = line.split(separator: " ", maxSplits: 1, omittingEmptySubsequences: true)
            guard parts.count == 2,
                  let pid = pid_t(parts[0].trimmingCharacters(in: .whitespaces)) else { continue }
            let cmd = String(parts[1])
            let hit = (cmd.contains("mtpfuse") || cmd.contains("adbfuse")) && cmd.contains(path)
            if hit {
                pids.append(pid)
            }
        }
        return pids
    }

    private func resolvedMtpfusePath() -> String? {
        if let c = cachedMtpfusePath { return c }
        let fm = FileManager.default
        var candidates: [String] = []
        if let bundleHelper = Bundle.main.url(forAuxiliaryExecutable: "mtpfuse")?.path {
            candidates.append(bundleHelper)
        }
        if let res = Bundle.main.resourcePath {
            candidates.append("\(res)/mtpfuse")
        }
        let exeDir = Bundle.main.bundleURL.deletingLastPathComponent().path
        candidates.append("\(exeDir)/mtpfuse")
        candidates.append("/usr/local/bin/mtpfuse")
        candidates.append("/opt/homebrew/bin/mtpfuse")
        candidates.append(FileManager.default.currentDirectoryPath + "/mtpfuse")
        let found = candidates.first { fm.isExecutableFile(atPath: $0) }
        cachedMtpfusePath = found
        return found
    }

    private func resolvedAdbfusePath() -> String? {
        if let c = cachedAdbfusePath { return c }
        let fm = FileManager.default
        var candidates: [String] = []
        if let bundleHelper = Bundle.main.url(forAuxiliaryExecutable: "adbfuse")?.path {
            candidates.append(bundleHelper)
        }
        if let res = Bundle.main.resourcePath {
            candidates.append("\(res)/adbfuse")
        }
        let exeDir = Bundle.main.bundleURL.deletingLastPathComponent().path
        candidates.append("\(exeDir)/adbfuse")
        candidates.append(FileManager.default.currentDirectoryPath + "/build/adbfuse")
        let found = candidates.first { fm.isExecutableFile(atPath: $0) }
        cachedAdbfusePath = found
        return found
    }
}
