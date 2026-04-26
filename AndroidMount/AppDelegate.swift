import Cocoa
import UserNotifications

final class AppDelegate: NSObject, NSApplicationDelegate, UNUserNotificationCenterDelegate {
    private var statusItem: NSStatusItem!
    private var usbWatcher: USBWatcher?
    private var mountManager: MountManager?
    private var unmountObserver: NSObjectProtocol?
    private var sessionPollTimer: Timer?

    /// USB location → device info while connected (may mount async).
    private var knownDevices: [UInt32: USBDevice] = [:]
    /// Location IDs currently waiting on `MountManager.mount` (USB auto or menu “Mount”).
    private var mountingDevices: Set<UInt32> = []

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        statusItem.isVisible = false
        if let btn = statusItem.button {
            if let img = PhoneSymbol.image(pointSize: 15) {
                btn.image = img
                btn.image?.isTemplate = true
            } else {
                btn.title = "Phone"
            }
            if btn.image == nil { btn.title = "Phone" }
        }

        mountManager = MountManager()
        mergeAdoptedOrphanDevices()
        configureUserNotifications()

        usbWatcher = USBWatcher { [weak self] event in
            DispatchQueue.main.async { self?.handleUSB(event) }
        }

        unmountObserver = NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didUnmountNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            guard let self = self else { return }
            if self.mountManager?.reconcileStaleSessions() == true {
                self.rebuildMenu()
            }
        }

        let poll = Timer(timeInterval: 2.0, repeats: true) { [weak self] _ in
            guard let self = self,
                  let mm = self.mountManager,
                  !self.knownDevices.isEmpty,
                  mm.hasActiveMountSessions else { return }
            if mm.reconcileStaleSessions() {
                self.rebuildMenu()
            }
        }
        sessionPollTimer = poll
        RunLoop.main.add(poll, forMode: .common)

        rebuildMenu()

        if !FileManager.default.fileExists(atPath: "/Library/Filesystems/macfuse.fs") &&
           !FileManager.default.fileExists(atPath: "/Library/Filesystems/osxfuse.fs") {
            statusItem.isVisible = true
            showMacFuseAlert()
            rebuildMenu()
        }

        usbWatcher?.start()
    }

    /// After relaunch, FUSE may still be mounted while `knownDevices` is empty — scan `~/.AndroidMount`.
    private func mergeAdoptedOrphanDevices() {
        guard let mm = mountManager else { return }
        for row in mm.adoptOrphanMountsIfNeeded() {
            if knownDevices[row.locationID] != nil { continue }
            knownDevices[row.locationID] = USBDevice(
                name: row.displayName,
                vendorID: 0,
                productID: 0,
                locationID: row.locationID)
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        sessionPollTimer?.invalidate()
        sessionPollTimer = nil
        if let o = unmountObserver {
            NSWorkspace.shared.notificationCenter.removeObserver(o)
            unmountObserver = nil
        }
        usbWatcher?.stop()
        mountManager?.unmountBlockingForQuit()
    }

    private func handleUSB(_ event: USBEvent) {
        switch event {
        case .connected(let dev):
            knownDevices[dev.locationID] = dev
            mountingDevices.insert(dev.locationID)
            rebuildMenu()
            mountManager?.mount(device: dev) { [weak self] result in
                DispatchQueue.main.async {
                    guard let self = self else { return }
                    self.mountingDevices.remove(dev.locationID)
                    switch result {
                    case .success(let mountPath):
                        self.rebuildMenu()
                        self.postDeviceConnectedNotification(deviceName: dev.name, mountPath: mountPath)
                    case .failure(let err):
                        self.knownDevices.removeValue(forKey: dev.locationID)
                        let anyLeft = !self.knownDevices.isEmpty || !self.mountingDevices.isEmpty
                            || (self.mountManager?.hasActiveMountSessions ?? false)
                        if anyLeft {
                            self.rebuildMenu(error: err.localizedDescription, for: dev)
                        } else {
                            Self.presentMountFailureAlert(device: dev, error: err)
                            self.rebuildMenu()
                        }
                    }
                }
            }
        case .disconnected(let dev):
            knownDevices.removeValue(forKey: dev.locationID)
            mountManager?.unmount(locationID: dev.locationID)
            rebuildMenu()
        }
    }

    private func rebuildMenu(error: String? = nil, for failed: USBDevice? = nil) {
        let menu = NSMenu()
        mountManager?.reconcileStaleSessions()
        guard let mm = mountManager else {
            menu.addItem(withTitle: "Starting…", action: nil, keyEquivalent: "")
            menu.addItem(.separator())
            menu.addItem(withTitle: "Quit AndroidMount", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
            statusItem.menu = menu
            statusItem.isVisible = false
            return
        }

        if let err = error, let dev = failed {
            menu.addItem(withTitle: "Error (\(dev.name)): \(err)", action: nil, keyEquivalent: "")
            menu.addItem(.separator())
        }

        let sortedDevs = knownDevices.sorted {
            $0.value.name.localizedCaseInsensitiveCompare($1.value.name) == .orderedAscending
        }

        if sortedDevs.isEmpty {
            menu.addItem(withTitle: "No device", action: nil, keyEquivalent: "")
        } else {
            let header = NSMenuItem(title: "Devices", action: nil, keyEquivalent: "")
            header.isEnabled = false
            menu.addItem(header)
            for (loc, dev) in sortedDevs {
                let sub = NSMenu()
                let path = mm.mountPoint(for: loc)
                let isMounting = mountingDevices.contains(loc)

                if let mp = path {
                    let open = NSMenuItem(title: "Show in Finder", action: #selector(openMount(_:)), keyEquivalent: "o")
                    open.target = self
                    open.representedObject = mp
                    sub.addItem(open)
                    let ej = NSMenuItem(title: "Eject", action: #selector(ejectOne(_:)), keyEquivalent: "")
                    ej.target = self
                    ej.representedObject = NSNumber(value: loc)
                    sub.addItem(ej)
                } else if isMounting {
                    let pending = NSMenuItem(title: "Mounting…", action: nil, keyEquivalent: "")
                    pending.isEnabled = false
                    pending.image = Self.sfMenuIcon("arrow.triangle.2.circlepath")
                    sub.addItem(pending)
                } else {
                    let mountItem = NSMenuItem(title: "Mount", action: #selector(mountOne(_:)), keyEquivalent: "")
                    mountItem.target = self
                    mountItem.representedObject = NSNumber(value: loc)
                    mountItem.image = Self.sfMenuIcon("externaldrive.badge.plus")
                    sub.addItem(mountItem)
                }

                let item = NSMenuItem(title: dev.name, action: nil, keyEquivalent: "")
                item.submenu = sub
                item.image = Self.phoneMenuIcon()
                menu.addItem(item)
            }
            if sortedDevs.contains(where: { mm.mountPoint(for: $0.key) != nil }) {
                menu.addItem(.separator())
                let ejectAll = NSMenuItem(title: "Eject all", action: #selector(ejectAll), keyEquivalent: "e")
                ejectAll.target = self
                menu.addItem(ejectAll)
            }
        }

        menu.addItem(.separator())
        menu.addItem(withTitle: "Quit AndroidMount", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        statusItem.menu = menu
        syncStatusItemVisibility(hasMountManager: true)
    }

    /// Menubar agent: only show the icon when MTP-capable hardware is in play (or an active mount we track).
    private func syncStatusItemVisibility(hasMountManager: Bool) {
        guard hasMountManager else {
            statusItem.isVisible = false
            return
        }
        let active = !knownDevices.isEmpty
            || !mountingDevices.isEmpty
            || (mountManager?.hasActiveMountSessions ?? false)
        statusItem.isVisible = active
    }

    @objc private func openMount(_ sender: NSMenuItem) {
        guard let path = sender.representedObject as? String else { return }
        openMountInFinder(path: path)
    }

    private func openMountInFinder(path: String) {
        guard Self.isPathUnderAndroidMount(path),
              FileManager.default.fileExists(atPath: path) else { return }
        NSWorkspace.shared.open(URL(fileURLWithPath: path))
    }

    @objc private func ejectOne(_ sender: NSMenuItem) {
        if let n = sender.representedObject as? NSNumber {
            let loc = n.uint32Value
            mountManager?.unmount(locationID: loc)
            rebuildMenu()
        }
    }

    @objc private func ejectAll() {
        mountManager?.unmount()
        rebuildMenu()
    }

    @objc private func mountOne(_ sender: NSMenuItem) {
        guard let n = sender.representedObject as? NSNumber else { return }
        let loc = n.uint32Value
        guard let dev = knownDevices[loc] else { return }
        mountingDevices.insert(loc)
        rebuildMenu()
        mountManager?.mount(device: dev) { [weak self] result in
            DispatchQueue.main.async {
                guard let self = self else { return }
                self.mountingDevices.remove(loc)
                switch result {
                case .success(let mountPath):
                    self.rebuildMenu()
                    self.postDeviceConnectedNotification(deviceName: dev.name, mountPath: mountPath)
                case .failure(let err):
                    self.rebuildMenu(error: err.localizedDescription, for: dev)
                }
            }
        }
    }

    private static func sfMenuIcon(_ name: String) -> NSImage? {
        guard let raw = NSImage(systemSymbolName: name, accessibilityDescription: nil) else { return nil }
        let cfg = NSImage.SymbolConfiguration(pointSize: 13, weight: .regular)
        let img = raw.withSymbolConfiguration(cfg) ?? raw
        img.isTemplate = true
        let s = NSSize(width: 16, height: 16)
        img.size = s
        return img
    }

    private static func phoneMenuIcon() -> NSImage? {
        guard let img = PhoneSymbol.image(pointSize: 13) else { return nil }
        img.isTemplate = true
        img.size = NSSize(width: 16, height: 16)
        return img
    }

    private static func presentMountFailureAlert(device: USBDevice, error: Error) {
        let a = NSAlert()
        a.messageText = "Could not mount \(device.name)"
        a.informativeText = error.localizedDescription
        a.alertStyle = .warning
        a.runModal()
    }

    private static func isPathUnderAndroidMount(_ path: String) -> Bool {
        let root = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".AndroidMount", isDirectory: true).standardizedFileURL.path
        let p = URL(fileURLWithPath: path).standardizedFileURL.path
        if p == root { return true }
        return p.hasPrefix(root + "/")
    }

    // MARK: - User notifications

    private func configureUserNotifications() {
        let viewFinder = UNNotificationAction(
            identifier: Self.notificationActionViewFinder,
            title: "View in Finder",
            options: [.foreground])
        let category = UNNotificationCategory(
            identifier: Self.notificationCategoryDeviceMounted,
            actions: [viewFinder],
            intentIdentifiers: [],
            options: [])
        let center = UNUserNotificationCenter.current()
        center.setNotificationCategories([category])
        center.delegate = self
        center.requestAuthorization(options: [.alert, .sound]) { _, _ in }
    }

    private func postDeviceConnectedNotification(deviceName: String, mountPath: String) {
        let content = UNMutableNotificationContent()
        content.title = "\(deviceName) connected"
        content.body = "Your device is ready. You can browse files in Finder."
        content.sound = .default
        content.categoryIdentifier = Self.notificationCategoryDeviceMounted
        content.userInfo = [Self.notificationUserInfoPath: mountPath]

        let trigger = UNTimeIntervalNotificationTrigger(timeInterval: 0.05, repeats: false)
        let id = UUID().uuidString
        let request = UNNotificationRequest(identifier: id, content: content, trigger: trigger)
        UNUserNotificationCenter.current().add(request)
    }

    func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        willPresent notification: UNNotification,
        withCompletionHandler completion: @escaping (UNNotificationPresentationOptions) -> Void
    ) {
        completion([.banner, .sound])
    }

    func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        didReceive response: UNNotificationResponse,
        withCompletionHandler completion: @escaping () -> Void
    ) {
        defer { completion() }
        let info = response.notification.request.content.userInfo
        guard let path = info[Self.notificationUserInfoPath] as? String else { return }
        switch response.actionIdentifier {
        case UNNotificationDefaultActionIdentifier,
             Self.notificationActionViewFinder:
            openMountInFinder(path: path)
        default:
            break
        }
    }

    private static let notificationCategoryDeviceMounted = "DEVICE_MOUNTED"
    private static let notificationActionViewFinder = "VIEW_FINDER"
    private static let notificationUserInfoPath = "path"

    private func showMacFuseAlert() {
        let alert = NSAlert()
        alert.messageText = "macFUSE is required"
        alert.informativeText = "AndroidMount needs macFUSE to mount Android devices as Finder volumes.\n\nInstall it from https://osxfuse.github.io and reboot."
        alert.addButton(withTitle: "Open Download Page")
        alert.addButton(withTitle: "Continue Anyway")
        if alert.runModal() == .alertFirstButtonReturn,
           let url = URL(string: "https://osxfuse.github.io") {
            NSWorkspace.shared.open(url)
        }
    }
}
