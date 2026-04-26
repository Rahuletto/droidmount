import Foundation
import Darwin

/// One macFUSE + mtpfuse process per USB location (supports several phones at once).
final class MountManager {
    enum MountError: LocalizedError {
        case helperMissing
        case mountFailed(String)
        case mountpointBusy
        var errorDescription: String? {
            switch self {
            case .helperMissing:
                return "MTPFuse helper binary not found. Build the MTPFuse target."
            case .mountFailed(let s):
                return "Mount failed: \(s)"
            case .mountpointBusy:
                return "Mount point is already in use."
            }
        }
    }

    private struct Session {
        let mountPoint: String
        let process: Process
    }

    private let parentDir: String = "\(NSHomeDirectory())/.AndroidMount"
    /// Finder / sidebar volume icon (macFUSE `volicon=`).
    private let volumeIconPath =
        "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/com.apple.iphone.icns"

    private var sessions: [UInt32: Session] = [:]
    private let sessionsLock = NSLock()

    /// Mount one device; idempotent if already mounted for this `locationID`.
    func mount(device: USBDevice, completion: @escaping (Result<String, Error>) -> Void) {
        sessionsLock.lock()
        if let existing = sessions[device.locationID] {
            sessionsLock.unlock()
            completion(.success(existing.mountPoint))
            return
        }
        sessionsLock.unlock()

        guard let helper = locateHelper() else {
            completion(.failure(MountError.helperMissing))
            return
        }

        let safeDir = device.name
            .replacingOccurrences(of: "/", with: "_")
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

        let safeVol = device.name
            .replacingOccurrences(of: ",", with: "")
            .replacingOccurrences(of: " ", with: "_")

        /* No direct_io: Quick Look / mmap-style readers need normal page-cache
         * semantics; direct_io breaks many previews on macFUSE. */
        var fuseOpts =
            "noappledouble,noapplexattr,noatime," +
            "iosize=1048576,daemon_timeout=300," +
            "attr_timeout=3600,entry_timeout=3600,negative_timeout=3600" +
            ",volname=\(safeVol)"

        let fm = FileManager.default
        if fm.fileExists(atPath: volumeIconPath) {
            fuseOpts += ",volicon=\(volumeIconPath)"
        }

        let p = Process()
        p.executableURL = URL(fileURLWithPath: helper)
        p.arguments = ["-f", "-o", fuseOpts, mountPoint]

        var env = ProcessInfo.processInfo.environment
        env["DYLD_FALLBACK_LIBRARY_PATH"] =
            "/opt/homebrew/lib:/usr/local/lib:/Library/Frameworks/macFUSE.framework/Versions/A:" +
            (env["DYLD_FALLBACK_LIBRARY_PATH"] ?? "")
        env["MTP_USB_BUS_LOCATION"] = String(device.locationID)
        p.environment = env

        let errPipe = Pipe()
        p.standardError = errPipe
        p.standardOutput = Pipe()

        var didReturn = false
        let returnOnce: (Result<String, Error>) -> Void = { r in
            if !didReturn { didReturn = true; completion(r) }
        }

        p.terminationHandler = { proc in
            if !didReturn {
                let data = errPipe.fileHandleForReading.readDataToEndOfFile()
                let msg = String(data: data, encoding: .utf8) ?? "exit \(proc.terminationStatus)"
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
                        self.sessionsLock.lock()
                        self.sessions[device.locationID] = Session(mountPoint: mountPoint, process: p)
                        self.sessionsLock.unlock()
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
                returnOnce(.failure(MountError.mountFailed("timed out waiting for mount")))
            }
        } catch {
            completion(.failure(error))
        }

        let mp = mountPoint
        DispatchQueue.global().asyncAfter(deadline: .now() + 1.0) {
            let noIndex = "\(mp)/.metadata_never_index"
            _ = FileManager.default.createFile(atPath: noIndex, contents: nil)
            let task = Process()
            task.executableURL = URL(fileURLWithPath: "/usr/bin/xattr")
            task.arguments = ["-w", "-s", "com.apple.metadata.spotlight.indexing-disable", "1", mp]
            try? task.run()
            task.waitUntilExit()
        }
    }

    func unmount(locationID: UInt32? = nil) {
        sessionsLock.lock()
        let targets: [UInt32: Session]
        if let id = locationID {
            if let s = sessions.removeValue(forKey: id) {
                targets = [id: s]
            } else {
                targets = [:]
            }
        } else {
            targets = sessions
            sessions.removeAll()
        }
        sessionsLock.unlock()

        for (_, s) in targets {
            tearDown(session: s)
        }
    }

    private func tearDown(session: Session) {
        let mp = session.mountPoint
        let p = session.process

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

        for pid in mtpfusePids(usingMountPoint: mp) {
            kill(pid, SIGTERM)
        }
        Thread.sleep(forTimeInterval: 0.2)
        for pid in mtpfusePids(usingMountPoint: mp) {
            kill(pid, SIGKILL)
        }
        Thread.sleep(forTimeInterval: 0.15)

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

    // MARK: - Mount point helpers

    private func isFuseMounted(at path: String) -> Bool {
        var st = statfs()
        guard statfs(path, &st) == 0 else { return false }
        let fstype = withUnsafeBytes(of: &st.f_fstypename) { raw -> String in
            String(cString: raw.baseAddress!.assumingMemoryBound(to: CChar.self))
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
        for pid in mtpfusePids(usingMountPoint: path) {
            kill(pid, SIGTERM)
        }
        Thread.sleep(forTimeInterval: 0.4)
        for pid in mtpfusePids(usingMountPoint: path) {
            kill(pid, SIGKILL)
        }
        Thread.sleep(forTimeInterval: 0.2)
        runDiskutilUnmountForce(path)
    }

    private func runDiskutilUnmountForce(_ path: String) {
        let du = Process()
        du.executableURL = URL(fileURLWithPath: "/usr/sbin/diskutil")
        du.arguments = ["unmount", "force", path]
        du.standardOutput = Pipe(); du.standardError = Pipe()
        try? du.run(); du.waitUntilExit()
    }

    private func mtpfusePids(usingMountPoint path: String) -> [pid_t] {
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
            if cmd.contains("mtpfuse") && cmd.contains(path) {
                pids.append(pid)
            }
        }
        return pids
    }

    private func locateHelper() -> String? {
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
        return candidates.first { fm.isExecutableFile(atPath: $0) }
    }
}
