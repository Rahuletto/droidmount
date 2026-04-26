#!/usr/bin/env swift
/*
 * Build a standard macOS .iconset from a source image using aspect-fill into
 * each square size, then iconutil → .icns (no letterboxing).
 *
 * Usage: aspect_fill_icns.swift <input.(icns|png|…)> <output.icns>
 */
import AppKit
import Foundation

guard CommandLine.argc == 3 else {
    FileHandle.standardError.write(Data("usage: aspect_fill_icns <in> <out.icns>\n".utf8))
    exit(2)
}

let inPath = CommandLine.arguments[1]
let outPath = CommandLine.arguments[2]

guard let src = NSImage(contentsOfFile: inPath) else {
    FileHandle.standardError.write(Data("error: could not load image\n".utf8))
    exit(1)
}

/// Pixel sizes for filenames in a standard AppKit iconset.
let layers: [(filename: String, pixels: Int)] = [
    ("icon_16x16.png", 16),
    ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32),
    ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128),
    ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256),
    ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512),
    ("icon_512x512@2x.png", 1024),
]

func cgImage(from image: NSImage) -> CGImage? {
    var rect = CGRect(origin: .zero, size: image.size)
    return image.cgImage(forProposedRect: &rect, context: nil, hints: [
        .interpolation: NSImageInterpolation.high,
    ])
}

func pngData(aspectFill image: NSImage, pixel: Int) -> Data? {
    let w = pixel
    let h = pixel
    guard let cs = CGColorSpace(name: CGColorSpace.sRGB) else { return nil }
    guard let ctx = CGContext(
        data: nil,
        width: w,
        height: h,
        bitsPerComponent: 8,
        bytesPerRow: 0,
        space: cs,
        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
    ) else { return nil }

    ctx.interpolationQuality = .high
    ctx.clear(CGRect(x: 0, y: 0, width: w, height: h))

    let sz = image.size
    guard sz.width > 0, sz.height > 0 else { return nil }
    guard let cg = cgImage(from: image) else { return nil }

    let scale = max(CGFloat(w) / sz.width, CGFloat(h) / sz.height)
    let rw = sz.width * scale
    let rh = sz.height * scale
    let x = (CGFloat(w) - rw) / 2
    let y = (CGFloat(h) - rh) / 2

    ctx.draw(cg, in: CGRect(x: x, y: y, width: rw, height: rh))
    guard let outCg = ctx.makeImage() else { return nil }
    let rep = NSBitmapImageRep(cgImage: outCg)
    return rep.representation(using: .png, properties: [:])
}

let fm = FileManager.default
let tmp = fm.temporaryDirectory.appendingPathComponent("androidmount_icns_\(UUID().uuidString)", isDirectory: true)
let iconset = tmp.appendingPathComponent("Icon.iconset", isDirectory: true)

do {
    try fm.createDirectory(at: iconset, withIntermediateDirectories: true)
    for layer in layers {
        guard let data = pngData(aspectFill: src, pixel: layer.pixels) else {
            throw NSError(domain: "aspect_fill_icns", code: 1, userInfo: [NSLocalizedDescriptionKey: "render failed"])
        }
        try data.write(to: iconset.appendingPathComponent(layer.filename))
    }

    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
    p.arguments = ["-c", "icns", iconset.path, "-o", outPath]
    try p.run()
    p.waitUntilExit()
    if p.terminationStatus != 0 {
        FileHandle.standardError.write(Data("error: iconutil failed\n".utf8))
        exit(1)
    }
} catch {
    FileHandle.standardError.write(Data("error: \(error.localizedDescription)\n".utf8))
    exit(1)
}

try? fm.removeItem(at: tmp)
