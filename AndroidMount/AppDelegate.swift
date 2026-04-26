import Cocoa

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusItem: NSStatusItem!
    private var usbWatcher: USBWatcher!
    private var mountManager: MountManager!

    /// USB location → device info while connected (may mount async).
    private var knownDevices: [UInt32: USBDevice] = [:]

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let btn = statusItem.button {
            if let img = NSImage(systemSymbolName: "iphone",
                                 accessibilityDescription: "AndroidMount") {
                img.isTemplate = true
                btn.image = img
            } else {
                btn.title = "Phone"
            }
            if btn.image == nil { btn.title = "Phone" }
        }
        rebuildMenu()

        if !FileManager.default.fileExists(atPath: "/Library/Filesystems/macfuse.fs") &&
           !FileManager.default.fileExists(atPath: "/Library/Filesystems/osxfuse.fs") {
            showMacFuseAlert()
        }

        mountManager = MountManager()
        usbWatcher = USBWatcher { [weak self] event in
            DispatchQueue.main.async { self?.handleUSB(event) }
        }
        usbWatcher.start()
    }

    func applicationWillTerminate(_ notification: Notification) {
        usbWatcher?.stop()
        mountManager?.unmount()
    }

    private func handleUSB(_ event: USBEvent) {
        switch event {
        case .connected(let dev):
            knownDevices[dev.locationID] = dev
            rebuildMenu()
            mountManager.mount(device: dev) { [weak self] result in
                DispatchQueue.main.async {
                    guard let self = self else { return }
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
            mountManager.unmount(locationID: dev.locationID)
            rebuildMenu()
        }
    }

    private func rebuildMenu(error: String? = nil, for failed: USBDevice? = nil) {
        let menu = NSMenu()

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
                if let path = mountManager.mountPoint(for: loc) {
                    let sub = NSMenu()
                    let open = NSMenuItem(title: "Open in Finder", action: #selector(openMount(_:)), keyEquivalent: "o")
                    open.target = self
                    open.representedObject = path
                    sub.addItem(open)
                    let ej = NSMenuItem(title: "Eject", action: #selector(ejectOne(_:)), keyEquivalent: "")
                    ej.target = self
                    ej.representedObject = NSNumber(value: loc)
                    sub.addItem(ej)
                    let item = NSMenuItem(title: dev.name, action: nil, keyEquivalent: "")
                    item.submenu = sub
                    menu.addItem(item)
                } else {
                    let pending = NSMenuItem(title: "\(dev.name) — mounting…", action: nil, keyEquivalent: "")
                    pending.isEnabled = false
                    menu.addItem(pending)
                }
            }
            if sortedDevs.contains(where: { mountManager.mountPoint(for: $0.key) != nil }) {
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
            knownDevices.removeValue(forKey: loc)
            mountManager.unmount(locationID: loc)
            rebuildMenu()
        }
    }

    @objc private func ejectAll() {
        knownDevices.removeAll()
        mountManager.unmount()
        rebuildMenu()
    }

    private func showMacFuseAlert() {
        let alert = NSAlert()
        alert.messageText = "macFUSE is required"
        alert.informativeText = "AndroidMount needs macFUSE to mount Android devices as Finder volumes.\n\nInstall it from https://osxfuse.github.io and reboot."
        alert.addButton(withTitle: "Open Download Page")
        alert.addButton(withTitle: "Continue Anyway")
        if alert.runModal() == .alertFirstButtonReturn {
            NSWorkspace.shared.open(URL(string: "https://osxfuse.github.io")!)
        }
    }
}
