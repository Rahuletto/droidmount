import Cocoa

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusItem: NSStatusItem!
    private var usbWatcher: USBWatcher!
    private var mountManager: MountManager!
    private var currentDevice: USBDevice?

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Menu bar item
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let btn = statusItem.button {
            // Use SF Symbol - fall back to text if not available
            if let img = NSImage(systemSymbolName: "iphone",
                                 accessibilityDescription: "AndroidMount") {
                img.isTemplate = true
                btn.image = img
            } else {
                btn.title = "Android"
            }
            if btn.image == nil { btn.title = "Android" }
        }
        rebuildMenu(state: .idle)

        // Verify macFUSE
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
        mountManager?.unmount()
    }

    // MARK: - USB events
    private func handleUSB(_ event: USBEvent) {
        switch event {
        case .connected(let dev):
            guard currentDevice == nil else { return }
            currentDevice = dev
            rebuildMenu(state: .mounting(dev.name))
            mountManager.mount(deviceName: dev.name) { [weak self] result in
                DispatchQueue.main.async {
                    switch result {
                    case .success(let path):
                        self?.rebuildMenu(state: .mounted(dev.name, path))
                        // Don't auto-open Finder — let the user open it
                        // from the menu. Auto-opening immediately after
                        // mount can race with macFUSE's volume registration
                        // and trigger heavy thumbnail/indexing work.
                    case .failure(let err):
                        self?.rebuildMenu(state: .error(err.localizedDescription))
                    }
                }
            }
        case .disconnected(let dev):
            if currentDevice?.locationID == dev.locationID {
                currentDevice = nil
                mountManager.unmount()
                rebuildMenu(state: .idle)
            }
        }
    }

    // MARK: - Menu
    enum MenuState {
        case idle
        case mounting(String)
        case mounted(String, String)
        case error(String)
    }

    private func rebuildMenu(state: MenuState) {
        let menu = NSMenu()
        switch state {
        case .idle:
            menu.addItem(withTitle: "No Android device", action: nil, keyEquivalent: "")
        case .mounting(let name):
            menu.addItem(withTitle: "Mounting \(name)…", action: nil, keyEquivalent: "")
        case .mounted(let name, let path):
            menu.addItem(withTitle: "Connected", action: nil, keyEquivalent: "")
            menu.addItem(.separator())
            let reveal = NSMenuItem(title: "Open in Finder",
                                    action: #selector(openInFinder), keyEquivalent: "o")
            reveal.target = self
            reveal.representedObject = path
            menu.addItem(reveal)
            let eject = NSMenuItem(title: "Eject",
                                   action: #selector(eject), keyEquivalent: "e")
            eject.target = self
            menu.addItem(eject)
        case .error(let msg):
            menu.addItem(withTitle: "Error: \(msg)", action: nil, keyEquivalent: "")
        }
        menu.addItem(.separator())
        let quit = NSMenuItem(title: "Quit AndroidMount",
                              action: #selector(NSApplication.terminate(_:)),
                              keyEquivalent: "q")
        menu.addItem(quit)
        statusItem.menu = menu
    }

    @objc private func openInFinder(_ sender: NSMenuItem) {
        if let path = sender.representedObject as? String {
            NSWorkspace.shared.open(URL(fileURLWithPath: path))
        }
    }

    @objc private func eject() {
        mountManager.unmount()
        currentDevice = nil
        rebuildMenu(state: .idle)
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
