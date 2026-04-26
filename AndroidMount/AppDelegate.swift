import Cocoa
import UserNotifications

final class AppDelegate: NSObject, NSApplicationDelegate, UNUserNotificationCenterDelegate {
    private var statusItem: NSStatusItem!
    private var usbWatcher: USBWatcher?
    private var mountManager: MountManager?
    private var unmountObserver: NSObjectProtocol?

    /// USB location → device info while connected (may mount async).
    private var knownDevices: [UInt32: USBDevice] = [:]
    /// Location IDs currently waiting on `MountManager.mount` (USB auto or menu “Mount”).
    private var mountingDevices: Set<UInt32> = []
    /// Phone is connected but `mtpfuse` could not open MTP (often USB mode still “charge only”).
    private var mtpModePending: Set<UInt32> = []
    /// Keeps the slashed menubar icon through brief USB disconnect/reconnect (same port) without flashing plain.
    private var mtpMenubarSlashGraceUntil: [UInt32: Date] = [:]
    /// Don’t post another MTP-mode banner for this location until this date (USB often flaps connect/disconnect).
    private var mtpPendingNotifySuppressedUntil: [UInt32: Date] = [:]
    /// Minimum gap between automatic USB-triggered mounts for the same port (stops mount+failure loops).
    private var lastUsbAutomountStart: [UInt32: Date] = [:]
    /// Avoid re-applying the same menubar SF Symbol when state unchanged.
    private var lastMenubarSlashedForSymbol: Bool?

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        statusItem.isVisible = false
        if let btn = statusItem.button {
            Self.applyPhoneSymbol(to: btn, slashed: false)
        }

        mountManager = MountManager()
        mergeAdoptedOrphanDevices()
        applyApplicationIconFromBundle()
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
                self.rebuildMenu(reconcileSessions: false)
            }
        }

        rebuildMenu()

        let hasMacFuse = FileManager.default.fileExists(atPath: "/Library/Filesystems/macfuse.fs")
            || FileManager.default.fileExists(atPath: "/Library/Filesystems/osxfuse.fs")
        if !hasMacFuse {
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
            let loc = dev.locationID
            knownDevices[loc] = dev
            if mountingDevices.contains(loc) {
                rebuildMenu()
                return
            }
            let now = Date()
            if let last = lastUsbAutomountStart[loc], now.timeIntervalSince(last) < 10.0 {
                rebuildMenu()
                return
            }
            lastUsbAutomountStart[loc] = now
            mountingDevices.insert(loc)
            rebuildMenu()
            mountManager?.mount(device: dev) { [weak self] result in
                DispatchQueue.main.async {
                    guard let self = self else { return }
                    self.mountingDevices.remove(dev.locationID)
                    switch result {
                    case .success(let mountPath):
                        let loc = dev.locationID
                        self.mtpModePending.remove(loc)
                        self.mtpMenubarSlashGraceUntil.removeValue(forKey: loc)
                        self.mtpPendingNotifySuppressedUntil.removeValue(forKey: loc)
                        self.rebuildMenu()
                        self.postDeviceConnectedNotification(deviceName: dev.name, mountPath: mountPath)
                    case .failure(let err):
                        if Self.isLikelyMTPModePendingError(err) {
                            self.knownDevices[dev.locationID] = dev
                            self.mtpModePending.insert(dev.locationID)
                            self.mtpMenubarSlashGraceUntil[dev.locationID] =
                                Date().addingTimeInterval(Self.mtpMenubarSlashGraceSeconds)
                            self.rebuildMenu()
                            self.postMTPModePendingNotificationIfAllowed(
                                deviceName: dev.name,
                                locationID: dev.locationID)
                            return
                        }
                        self.mtpModePending.remove(dev.locationID)
                        self.mtpMenubarSlashGraceUntil.removeValue(forKey: dev.locationID)
                        self.mtpPendingNotifySuppressedUntil.removeValue(forKey: dev.locationID)
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
            let loc = dev.locationID
            knownDevices.removeValue(forKey: loc)
            mtpModePending.remove(loc)
            lastUsbAutomountStart.removeValue(forKey: loc)
            mountManager?.unmount(locationID: loc)
            rebuildMenu()
        }
    }

    private func rebuildMenu(error: String? = nil, for failed: USBDevice? = nil, reconcileSessions: Bool = true) {
        let menu = NSMenu()
        if reconcileSessions {
            mountManager?.reconcileStaleSessions()
        }
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
            var anyMounted = false
            for (loc, dev) in sortedDevs {
                let sub = NSMenu()
                let path = mm.mountPoint(for: loc)
                let isMounting = mountingDevices.contains(loc)
                let pendingMtp = mtpModePending.contains(loc)

                if isMounting {
                    let pending = NSMenuItem(title: "Mounting…", action: nil, keyEquivalent: "")
                    pending.isEnabled = false
                    pending.image = Self.cachedSfMenuIconSpinner()
                    sub.addItem(pending)
                } else if let mp = path {
                    anyMounted = true
                    let open = NSMenuItem(title: "Show in Finder", action: #selector(openMount(_:)), keyEquivalent: "o")
                    open.target = self
                    open.representedObject = mp
                    sub.addItem(open)
                    let ej = NSMenuItem(title: "Eject", action: #selector(ejectOne(_:)), keyEquivalent: "")
                    ej.target = self
                    ej.representedObject = NSNumber(value: loc)
                    sub.addItem(ej)
                    let tr = mm.transport(for: loc)
                    if tr == .mtpHybrid || tr == .adb {
                        let adbLabel = NSMenuItem(title: "Using ADB", action: nil, keyEquivalent: "")
                        adbLabel.isEnabled = false
                        sub.addItem(adbLabel)
                    } else {
                        let req = NSMenuItem(
                            title: "Request ADB mode",
                            action: #selector(requestAdbOne(_:)),
                            keyEquivalent: "")
                        req.target = self
                        req.representedObject = NSNumber(value: loc)
                        sub.addItem(req)
                    }
                } else {
                    if pendingMtp {
                        let hint = NSMenuItem(
                            title: "Not in MTP mode — on the phone choose File transfer (MTP)",
                            action: nil,
                            keyEquivalent: "")
                        hint.isEnabled = false
                        sub.addItem(hint)
                    }
                    let mountTitle = pendingMtp ? "Retry mount" : "Mount"
                    let mountItem = NSMenuItem(title: mountTitle, action: #selector(mountOne(_:)), keyEquivalent: "")
                    mountItem.target = self
                    mountItem.representedObject = NSNumber(value: loc)
                    mountItem.image = Self.cachedSfMenuIconMount()
                    sub.addItem(mountItem)
                }

                let item = NSMenuItem(title: dev.name, action: nil, keyEquivalent: "")
                item.submenu = sub
                item.image = pendingMtp ? Self.slashedPhoneMenuIcon() : Self.phoneMenuIcon()
                menu.addItem(item)
            }
            if anyMounted {
                menu.addItem(.separator())
                let ejectAll = NSMenuItem(title: "Eject all", action: #selector(ejectAll), keyEquivalent: "e")
                ejectAll.target = self
                menu.addItem(ejectAll)
            }
        }

        menu.addItem(withTitle: "Quit AndroidMount", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        statusItem.menu = menu
        syncStatusItemVisibility(hasMountManager: true)
        refreshStatusItemSymbol()
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

    private func refreshStatusItemSymbol() {
        guard let btn = statusItem.button else { return }
        let slashed = shouldShowSlashedMenubarIcon()
        if lastMenubarSlashedForSymbol == slashed { return }
        lastMenubarSlashedForSymbol = slashed
        Self.applyPhoneSymbol(to: btn, slashed: slashed)
    }

    private func shouldShowSlashedMenubarIcon() -> Bool {
        if !mtpModePending.isEmpty { return true }
        guard let mm = mountManager else { return false }
        let now = Date()
        for (loc, _) in knownDevices {
            guard let until = mtpMenubarSlashGraceUntil[loc], now < until else { continue }
            if mm.mountPoint(for: loc) != nil { continue }
            return true
        }
        return false
    }

    /// MTP-pending: SF Symbol slash / alert variants via `PhoneSymbol` (native vectors, no bitmap compositing).
    private static func applyPhoneSymbol(to button: NSButton, slashed: Bool) {
        let plainSize: CGFloat = 16
        if let img = PhoneSymbol.image(pointSize: plainSize, slashed: slashed) {
            button.image = img
            button.image?.isTemplate = true
            button.title = ""
        } else {
            button.image = nil
            button.title = slashed ? "MTP" : "Phone"
        }
    }

    /// Heuristic: `mtpfuse` exited early because libmtp could not see an MTP session (wrong USB mode, locked, etc.).
    private static func isLikelyMTPModePendingError(_ error: Error) -> Bool {
        let text = error.localizedDescription.lowercased()
        if text.contains("out of memory") || text.contains("node_new(root)") { return false }
        let markers = [
            "failed to open mtp device",
            "no mtp device found",
            "file transfer mode",
            "no device with bus_location",
            "libmtp_open_raw_device failed",
            "is the phone unlocked",
            ", n=0",
            "no storage is visible",
            "no storage visible",
        ]
        if markers.contains(where: { text.contains($0) }) { return true }
        if text.contains("timed out waiting for fuse mount"),
           text.contains("detect returned") || text.contains("no mtp") {
            return true
        }
        return false
    }

    @objc private func openMount(_ sender: NSMenuItem) {
        guard let path = sender.representedObject as? String else { return }
        openMountInFinder(path: path)
    }

    private func openMountInFinder(path: String) {
        guard isPathUnderAndroidMount(path),
              FileManager.default.fileExists(atPath: path) else { return }
        let url = URL(fileURLWithPath: path, isDirectory: true)
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }

    @objc private func requestAdbOne(_ sender: NSMenuItem) {
        guard let n = sender.representedObject as? NSNumber else { return }
        let loc = n.uint32Value
        guard let dev = knownDevices[loc] else { return }
        mountingDevices.insert(loc)
        rebuildMenu()
        mountManager?.requestAdbMode(device: dev) { [weak self] result in
            DispatchQueue.main.async {
                guard let self = self else { return }
                self.mountingDevices.remove(loc)
                switch result {
                case .success(let r):
                    self.mtpModePending.remove(loc)
                    self.mtpMenubarSlashGraceUntil.removeValue(forKey: loc)
                    self.mtpPendingNotifySuppressedUntil.removeValue(forKey: loc)
                    self.rebuildMenu()
                    if let msg = r.mtpFallbackMessage {
                        Self.presentInformativeAlert(title: "ADB mode", message: msg)
                    }
                case .failure(let err):
                    self.mtpModePending.remove(loc)
                    self.mtpMenubarSlashGraceUntil.removeValue(forKey: loc)
                    self.mtpPendingNotifySuppressedUntil.removeValue(forKey: loc)
                    self.rebuildMenu(error: err.localizedDescription, for: dev)
                }
            }
        }
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
                    self.mtpModePending.remove(loc)
                    self.mtpMenubarSlashGraceUntil.removeValue(forKey: loc)
                    self.mtpPendingNotifySuppressedUntil.removeValue(forKey: loc)
                    self.rebuildMenu()
                    self.postDeviceConnectedNotification(deviceName: dev.name, mountPath: mountPath)
                case .failure(let err):
                    if Self.isLikelyMTPModePendingError(err) {
                        self.mtpModePending.insert(loc)
                        self.mtpMenubarSlashGraceUntil[loc] =
                            Date().addingTimeInterval(Self.mtpMenubarSlashGraceSeconds)
                        self.rebuildMenu()
                        self.postMTPModePendingNotificationIfAllowed(deviceName: dev.name, locationID: loc)
                    } else {
                        self.mtpModePending.remove(loc)
                        self.mtpMenubarSlashGraceUntil.removeValue(forKey: loc)
                        self.mtpPendingNotifySuppressedUntil.removeValue(forKey: loc)
                        self.rebuildMenu(error: err.localizedDescription, for: dev)
                    }
                }
            }
        }
    }

    private static let menuIconLock = NSLock()
    private static var menuIconSpinner: NSImage?
    private static var menuIconMount: NSImage?

    private static func cachedSfMenuIconSpinner() -> NSImage? {
        menuIconLock.lock()
        defer { menuIconLock.unlock() }
        if menuIconSpinner == nil {
            menuIconSpinner = sfMenuIconUncached("arrow.triangle.2.circlepath")
        }
        return menuIconSpinner.flatMap { $0.copy() as? NSImage }
    }

    private static func cachedSfMenuIconMount() -> NSImage? {
        menuIconLock.lock()
        defer { menuIconLock.unlock() }
        if menuIconMount == nil {
            menuIconMount = sfMenuIconUncached("externaldrive.badge.plus")
        }
        return menuIconMount.flatMap { $0.copy() as? NSImage }
    }

    private static func sfMenuIconUncached(_ name: String) -> NSImage? {
        guard let raw = NSImage(systemSymbolName: name, accessibilityDescription: nil) else { return nil }
        let cfg = NSImage.SymbolConfiguration(pointSize: 13, weight: .regular)
        let img = raw.withSymbolConfiguration(cfg) ?? raw
        img.isTemplate = true
        img.size = NSSize(width: 16, height: 16)
        return img
    }

    private static func phoneMenuIcon() -> NSImage? {
        guard let img = PhoneSymbol.image(pointSize: 13) else { return nil }
        img.isTemplate = true
        img.size = NSSize(width: 16, height: 16)
        return img
    }

    private static func slashedPhoneMenuIcon() -> NSImage? {
        guard let img = PhoneSymbol.image(pointSize: 13, slashed: true) else { return phoneMenuIcon() }
        img.size = NSSize(width: 16, height: 16)
        return img
    }

    private static func presentInformativeAlert(title: String, message: String) {
        let a = NSAlert()
        a.messageText = title
        a.informativeText = message
        a.alertStyle = .informational
        a.runModal()
    }

    private static func presentMountFailureAlert(device: USBDevice, error: Error) {
        let a = NSAlert()
        a.messageText = "Could not mount \(device.name)"
        a.informativeText = error.localizedDescription
        a.alertStyle = .warning
        a.runModal()
    }

    private func isPathUnderAndroidMount(_ path: String) -> Bool {
        let p = URL(fileURLWithPath: path).resolvingSymlinksInPath().path
        let root = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".AndroidMount", isDirectory: true)
            .resolvingSymlinksInPath().path
        if p == root { return true }
        let prefix = root.hasSuffix("/") ? root : root + "/"
        return p.hasPrefix(prefix)
    }

    // MARK: - User notifications

    /// Helps some contexts (e.g. notifications) pick up artwork; `CFBundleIconFile` is still the bundle app icon.
    private func applyApplicationIconFromBundle() {
        guard let url = Bundle.main.url(forResource: "icon", withExtension: "icns"),
              let image = NSImage(contentsOf: url),
              let filled = Self.nsImageAspectFill(from: image, pixelSide: 512) else { return }
        NSApp.applicationIconImage = filled
    }

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

    private static let mtpPendingBannerQuietSeconds: TimeInterval = 150
    private static let mtpMenubarSlashGraceSeconds: TimeInterval = 180

    private func postMTPModePendingNotificationIfAllowed(deviceName: String, locationID: UInt32) {
        let now = Date()
        if let until = mtpPendingNotifySuppressedUntil[locationID], now < until {
            return
        }
        mtpPendingNotifySuppressedUntil[locationID] = now.addingTimeInterval(Self.mtpPendingBannerQuietSeconds)
        postMTPModePendingNotification(deviceName: deviceName, locationID: locationID)
    }

    private func postMTPModePendingNotification(deviceName: String, locationID: UInt32) {
        let content = UNMutableNotificationContent()
        content.title = "\(deviceName): not in MTP mode yet"
        content.body = "On the phone, switch USB to File transfer (MTP) to access files. Then choose Retry mount in the AndroidMount menu bar."
        content.sound = .default

        let trigger = UNTimeIntervalNotificationTrigger(timeInterval: 0.05, repeats: false)
        let id = "mtp-pending-\(locationID)"
        let center = UNUserNotificationCenter.current()
        center.removeDeliveredNotifications(withIdentifiers: [id])
        center.removePendingNotificationRequests(withIdentifiers: [id])
        let request = UNNotificationRequest(identifier: id, content: content, trigger: trigger)
        center.add(request)
    }

    private func postDeviceConnectedNotification(deviceName: String, mountPath: String) {
        let content = UNMutableNotificationContent()
        content.title = "\(deviceName) connected"
        content.body =
            "Your device is ready. Use \"View in Finder\" here or Show in Finder in the menu bar when you want to open it."
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

    /// Scale + center-crop so artwork fills the square (removes empty transparent “padding” in source icons).
    private static func nsImageAspectFill(from source: NSImage, pixelSide: Int) -> NSImage? {
        guard let rep = bitmapAspectFillRep(from: source, pixelSide: pixelSide) else { return nil }
        let side = CGFloat(pixelSide)
        let img = NSImage(size: NSSize(width: side, height: side))
        img.addRepresentation(rep)
        return img
    }

    private static func bitmapAspectFillRep(from source: NSImage, pixelSide: Int) -> NSBitmapImageRep? {
        let side = CGFloat(pixelSide)
        let imageSize = source.size
        guard imageSize.width > 0, imageSize.height > 0,
              let rep = NSBitmapImageRep(
                bitmapDataPlanes: nil,
                pixelsWide: pixelSide,
                pixelsHigh: pixelSide,
                bitsPerSample: 8,
                samplesPerPixel: 4,
                hasAlpha: true,
                isPlanar: false,
                colorSpaceName: .deviceRGB,
                bytesPerRow: 0,
                bitsPerPixel: 0) else { return nil }
        rep.size = NSSize(width: side, height: side)
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
        NSGraphicsContext.current?.imageInterpolation = .high
        let scale = max(side / imageSize.width, side / imageSize.height)
        let w = imageSize.width * scale
        let h = imageSize.height * scale
        let x = (side - w) / 2
        let y = (side - h) / 2
        source.draw(
            in: NSRect(x: x, y: y, width: w, height: h),
            from: NSRect(origin: .zero, size: imageSize),
            operation: .copy,
            fraction: 1.0,
            respectFlipped: false,
            hints: [.interpolation: NSImageInterpolation.high])
        NSGraphicsContext.restoreGraphicsState()
        return rep
    }

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
