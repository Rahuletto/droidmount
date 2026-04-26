import Foundation
import Darwin

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
    // Hidden by default (prefixed with .)
    private let parentDir: String = "\(NSHomeDirectory())/.AndroidMount"
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

        // Ensure mountpoint exists; reclaim stale FUSE mounts from a prior crash / quit
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
        //   * NO local — prevent Finder sidebar/auto-indexing
        //   * direct_io,noatime — reduce overhead
        //   * long timeouts for slow MTP
        p.arguments = [
            "-f",
            "-o",
            "direct_io,noappledouble,noapplexattr,noatime,max_readahead=0," +
            "iosize=1048576,daemon_timeout=300," +
            "attr_timeout=3600,entry_timeout=3600,negative_timeout=3600" +
            ",volname=\(safe)",
            mountPoint
        ]

        // Disable Spotlight indexing on the mount point after mount
        let mp = mountPoint
        DispatchQueue.global().asyncAfter(deadline: .now() + 1.0) {
            // Create .no_index to tell Spotlight to skip
            let noIndex = "\(mp)/.metadata_never_index"
            _ = FileManager.default.createFile(atPath: noIndex, contents: nil)
            // Set xattr to exclude from Spotlight
            let task = Process()
            task.executableURL = URL(fileURLWithPath: "/usr/bin/xattr")
            task.arguments = ["-w", "-s", "com.apple.metadata.spotlight.indexing-disable", "1", mp]
            try? task.run()
            task.waitUntilExit()
        }

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
        let mp = mountPoint  // capture before we nil it
        // Try a clean unmount via diskutil first
        let du = Process()
        du.executableURL = URL(fileURLWithPath: "/usr/sbin/diskutil")
        du.arguments = ["unmount", "force", mp]
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

        // Clean up the mount directory after unmount
        DispatchQueue.global().asyncAfter(deadline: .now() + 0.5) {
            try? FileManager.default.removeItem(atPath: mp)
        }
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
        // Stale macFUSE mount: app restarted but mtpfuse still holds the path
        if isMounted() {
            reclaimStaleFuseMount(at: mountPoint)
            if isMounted() {
                throw MountError.mountpointBusy
            }
        }
    }

    /// Force-unmount and kill orphaned `mtpfuse` for this path (e.g. after Finder freeze / crash).
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
        // Drop stale helper reference if it was ours but the process table still matched
        if let p = process, !p.isRunning {
            process = nil
        }
    }

    private func runDiskutilUnmountForce(_ path: String) {
        let du = Process()
        du.executableURL = URL(fileURLWithPath: "/usr/sbin/diskutil")
        du.arguments = ["unmount", "force", path]
        du.standardOutput = Pipe()
        du.standardError = Pipe()
        try? du.run()
        du.waitUntilExit()
    }

    /// PIDs of `mtpfuse` whose argv contains this exact mount path (not AndroidMount itself).
    private func mtpfusePids(usingMountPoint path: String) -> [pid_t] {
        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/bin/ps")
        task.arguments = ["-axo", "pid=,command="]
        let out = Pipe()
        task.standardOutput = out
        task.standardError = Pipe()
        do {
            try task.run()
        } catch {
            return []
        }
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
        // Dev fallback: alongside the project
        candidates.append(FileManager.default.currentDirectoryPath + "/mtpfuse")
        return candidates.first { fm.isExecutableFile(atPath: $0) }
    }
}
