import Cocoa

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusItem: NSStatusItem!
    private var usbWatcher: USBWatcher?
    private var mountManager: MountManager?

    /// USB location → device info while connected (may mount async).
    private var knownDevices: [UInt32: USBDevice] = [:]
    /// Location IDs currently waiting on `MountManager.mount` (USB auto or menu “Mount”).
    private var mountingDevices: Set<UInt32> = []

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let btn = statusItem.button {
            if let img = NSImage(systemSymbolName: "iphone",
                                 accessibilityDescription: "AndroidMount") {
                let cfg = NSImage.SymbolConfiguration(pointSize: 15, weight: .regular)
                btn.image = img.withSymbolConfiguration(cfg)
                btn.image?.isTemplate = true
            } else {
                btn.title = "Phone"
            }
            if btn.image == nil { btn.title = "Phone" }
        }

        mountManager = MountManager()
        usbWatcher = USBWatcher { [weak self] event in
            DispatchQueue.main.async { self?.handleUSB(event) }
        }
        rebuildMenu()

        if !FileManager.default.fileExists(atPath: "/Library/Filesystems/macfuse.fs") &&
           !FileManager.default.fileExists(atPath: "/Library/Filesystems/osxfuse.fs") {
            showMacFuseAlert()
        }

        usbWatcher?.start()
    }

    func applicationWillTerminate(_ notification: Notification) {
        usbWatcher?.stop()
        mountManager?.unmount()
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
                    case .success:
                        self.rebuildMenu()
                    case .failure(let err):
                        self.knownDevices.removeValue(forKey: dev.locationID)
                        self.rebuildMenu(error: err.localizedDescription, for: dev)
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
        guard let mm = mountManager else {
            menu.addItem(withTitle: "Starting…", action: nil, keyEquivalent: "")
            menu.addItem(.separator())
            menu.addItem(withTitle: "Quit AndroidMount", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
            statusItem.menu = menu
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
                item.image = Self.sfMenuIcon("iphone")
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
    }

    @objc private func openMount(_ sender: NSMenuItem) {
        if let path = sender.representedObject as? String {
            NSWorkspace.shared.open(URL(fileURLWithPath: path))
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
                case .success:
                    self.rebuildMenu()
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
