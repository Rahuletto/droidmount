import Foundation

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

    // /Volumes is root-owned on modern macOS; use a path the user owns.
    // The macFUSE `local` + `volname` options still make Finder show
    // this as a normal sidebar volume. Each device gets its own
    // subdirectory named after the phone, e.g. ~/AndroidMount/Find 5.
    private let parentDir: String = "\(NSHomeDirectory())/AndroidMount"
    private var mountPoint: String = ""
    private var process: Process?

    func mount(deviceName: String, completion: @escaping (Result<String, Error>) -> Void) {
        guard let helper = locateHelper() else {
            completion(.failure(MountError.helperMissing))
            return
        }

        // Per-device subdirectory: ~/AndroidMount/<DeviceName>
        let safeDir = deviceName
            .replacingOccurrences(of: "/", with: "_")
            .trimmingCharacters(in: .whitespaces)
        mountPoint = "\(parentDir)/\(safeDir.isEmpty ? "Device" : safeDir)"

        // Ensure mountpoint exists and is empty
        do { try ensureMountpoint() } catch {
            completion(.failure(error)); return
        }

        let p = Process()
        p.executableURL = URL(fileURLWithPath: helper)
        // -f keeps fuse in foreground so process termination unmounts it.
        // `local` makes Finder treat it as a real local volume (sidebar).
        // Volume name is sanitized for FUSE option parsing (no commas/spaces).
        let safe = deviceName
            .replacingOccurrences(of: ",", with: "")
            .replacingOccurrences(of: " ", with: "_")
        // Important option choices for MTP-via-FUSE:
        //   * NO `local` — `local` makes Finder treat the mount as a
        //     fast local disk and triggers Spotlight indexing,
        //     thumbnail generation and .DS_Store writes that hammer the
        //     phone and freeze Finder. Treat it as a slow/remote vol.
        //   * `noappledouble`, `noapplexattr` — suppress ._foo metadata
        //   * `daemon_timeout=600` — MTP can be slow; default 60 s
        //     causes macFUSE to kill us on big folder listings
        //   * `attr_timeout=30,entry_timeout=30,negative_timeout=30` —
        //     let the kernel cache stat/lookup results so Finder
        //     doesn't re-walk the tree on every refresh
        //   * `iosize=1048576` — 1 MiB IO chunks
        //   * `noatime` — avoid useless mtime updates
        p.arguments = [
            "-f",
            "-o",
            "noappledouble,noapplexattr,noatime,iosize=1048576," +
            "daemon_timeout=600,attr_timeout=30,entry_timeout=30," +
            "negative_timeout=30,volname=\(safe)",
            mountPoint
        ]

        // Inherit env so DYLD finds libmtp/macfuse
        var env = ProcessInfo.processInfo.environment
        env["DYLD_FALLBACK_LIBRARY_PATH"] =
            "/opt/homebrew/lib:/usr/local/lib:/Library/Frameworks/macFUSE.framework/Versions/A:" +
            (env["DYLD_FALLBACK_LIBRARY_PATH"] ?? "")
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
            self.process = p
            // Poll for the mount up to ~8s
            DispatchQueue.global().async {
                for _ in 0..<40 {
                    Thread.sleep(forTimeInterval: 0.2)
                    if self.isMounted() {
                        returnOnce(.success(self.mountPoint))
                        return
                    }
                    if !p.isRunning { break }
                }
                returnOnce(.failure(MountError.mountFailed("timed out waiting for mount")))
            }
        } catch {
            completion(.failure(error))
        }
    }

    func unmount() {
        // Try a clean unmount via diskutil first
        let du = Process()
        du.executableURL = URL(fileURLWithPath: "/usr/sbin/diskutil")
        du.arguments = ["unmount", "force", mountPoint]
        du.standardOutput = Pipe(); du.standardError = Pipe()
        try? du.run(); du.waitUntilExit()

        if let p = process, p.isRunning {
            p.terminate()
            // give it a beat, then SIGKILL
            DispatchQueue.global().asyncAfter(deadline: .now() + 1.0) {
                if p.isRunning { kill(p.processIdentifier, SIGKILL) }
            }
        }
        process = nil
    }

    private func isMounted() -> Bool {
        var st = statfs()
        guard statfs(mountPoint, &st) == 0 else { return false }
        // After macFUSE mounts, f_fstypename becomes "macfuse" (or
        // "osxfuse" on legacy versions). The underlying APFS dir would
        // report "apfs". This check works regardless of how macFUSE
        // normalizes the mount path string.
        let fstype = withUnsafeBytes(of: &st.f_fstypename) { raw -> String in
            String(cString: raw.baseAddress!.assumingMemoryBound(to: CChar.self))
        }
        return fstype.contains("fuse") || fstype.contains("macfuse")
    }

    private func ensureMountpoint() throws {
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
        // If already a mountpoint, fail loudly
        if isMounted() {
            throw MountError.mountpointBusy
        }
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
        // Dev fallback: alongside the project
        candidates.append(FileManager.default.currentDirectoryPath + "/mtpfuse")
        return candidates.first { fm.isExecutableFile(atPath: $0) }
    }
}
