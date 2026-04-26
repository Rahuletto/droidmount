import Cocoa

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
// .accessory == menu bar app, no Dock icon, no main window required
app.setActivationPolicy(.accessory)
app.run()
