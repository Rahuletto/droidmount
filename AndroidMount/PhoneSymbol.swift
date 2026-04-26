import AppKit

/// Resolves the best available iPhone / smartphone SF Symbol for this OS (newest silhouettes first).
enum PhoneSymbol {
    /// Full-screen style → legacy `iphone` → generic handset.
    static let sfSymbolNameCandidates: [String] = [
        "iphone.gen3",
        "iphone.gen2",
        "iphone.gen1",
        "iphone",
        "smartphone",
    ]

    static func image(pointSize: CGFloat) -> NSImage? {
        let cfg = NSImage.SymbolConfiguration(pointSize: pointSize, weight: .regular)
        return image(symbolConfiguration: cfg)
    }

    static func image(symbolConfiguration cfg: NSImage.SymbolConfiguration) -> NSImage? {
        for name in sfSymbolNameCandidates {
            guard let raw = NSImage(systemSymbolName: name, accessibilityDescription: "Phone") else { continue }
            if let out = raw.withSymbolConfiguration(cfg) { return out }
        }
        return nil
    }
}
