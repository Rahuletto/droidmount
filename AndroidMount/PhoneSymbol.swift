import AppKit

/// Resolves the best available iPhone / smartphone SF Symbol for this OS (newest silhouettes first).
enum PhoneSymbol {
    static let sfSymbolNameCandidates: [String] = [
        "iphone.gen3",
        "iphone.gen2",
        "iphone.gen1",
        "iphone",
        "smartphone",
    ]

    static let sfSymbolSlashedCandidates: [String] = [
        "iphone.gen3.slash",
        "iphone.gen2.slash",
        "iphone.gen1.slash",
        "iphone.slash",
        "smartphone.slash",
        "exclamationmark.iphone",
        "iphone.radiowaves.left.and.right.slash",
    ]

    private static let cacheLock = NSLock()
    private static var plain13: NSImage?
    private static var plain16: NSImage?
    private static var slash13: NSImage?
    private static var slash16: NSImage?

    static func image(pointSize: CGFloat) -> NSImage? {
        let cfg = NSImage.SymbolConfiguration(pointSize: pointSize, weight: .regular)
        if pointSize == 13 {
            return copyOrBuild(&plain13) { image(symbolConfiguration: cfg) }
        }
        if pointSize == 16 {
            return copyOrBuild(&plain16) { image(symbolConfiguration: cfg) }
        }
        return image(symbolConfiguration: cfg)
    }

    static func image(pointSize: CGFloat, slashed: Bool) -> NSImage? {
        if !slashed { return image(pointSize: pointSize) }
        let cfg = NSImage.SymbolConfiguration(pointSize: pointSize, weight: .regular)
        if pointSize == 13 {
            return copyOrBuild(&slash13) { imageSlashed(symbolConfiguration: cfg) }
        }
        if pointSize == 16 {
            return copyOrBuild(&slash16) { imageSlashed(symbolConfiguration: cfg) }
        }
        return imageSlashed(symbolConfiguration: cfg)
    }

    /// `NSImage.isTemplate` is mutated by controls — hand out copies so cached images stay immutable.
    private static func copyOrBuild(_ slot: inout NSImage?, build: () -> NSImage?) -> NSImage? {
        cacheLock.lock()
        defer { cacheLock.unlock() }
        if slot == nil {
            slot = build()
        }
        return slot.flatMap { $0.copy() as? NSImage }
    }

    static func image(symbolConfiguration cfg: NSImage.SymbolConfiguration) -> NSImage? {
        for name in sfSymbolNameCandidates {
            guard let raw = NSImage(systemSymbolName: name, accessibilityDescription: "Phone") else { continue }
            if let out = raw.withSymbolConfiguration(cfg) { return out }
        }
        return nil
    }

    private static func imageSlashed(symbolConfiguration cfg: NSImage.SymbolConfiguration) -> NSImage? {
        for name in sfSymbolSlashedCandidates {
            guard let raw = NSImage(systemSymbolName: name, accessibilityDescription: "Phone not available")
            else { continue }
            if let out = raw.withSymbolConfiguration(cfg) { return out }
        }
        return image(symbolConfiguration: cfg)
    }
}
