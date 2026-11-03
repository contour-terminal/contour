#!/usr/bin/env swift
// SPDX-License-Identifier: Apache-2.0
//
// Regenerates src/contour/res/images/contour-logo.icns from contour-logo.png.
//
// macOS app icons follow Apple's icon grid: the artwork fills about 80.5% of the canvas (824 of 1024 px) and
// the rest is transparent margin. A full-bleed icon looks oversized next to every other icon in the Cmd-Tab
// switcher and the Dock, so the margin is applied here rather than baked into the source image.
//
// Usage (macOS only; needs the Xcode command line tools for swift and iconutil):
//
//     scripts/generate-macos-icns.swift [SOURCE_PNG [OUTPUT_ICNS]]
//
// Defaults: src/contour/res/images/contour-logo-512.png -> src/contour/res/images/contour-logo.icns
// The 512 px rendition is the source because it is the largest the repository holds (contour-logo.png is
// 256 px): the 1024 px member then scales it 1.6x rather than 3.2x.

import AppKit
import Foundation

/// Fraction of the canvas the artwork covers (Apple's icon grid: 824 / 1024).
let artworkFraction = 824.0 / 1024.0

/// The iconset members iconutil expects: (point size, scale).
let members: [(points: Int, scale: Int)] = [
    (16, 1), (16, 2), (32, 1), (32, 2), (128, 1), (128, 2), (256, 1), (256, 2), (512, 1), (512, 2),
]

let imagesDir = "src/contour/res/images"
let arguments = CommandLine.arguments
let sourcePath = arguments.count > 1 ? arguments[1] : "\(imagesDir)/contour-logo-512.png"
let outputPath = arguments.count > 2 ? arguments[2] : "\(imagesDir)/contour-logo.icns"

guard let source = NSImage(contentsOfFile: sourcePath) else {
    FileHandle.standardError.write("cannot read \(sourcePath)\n".data(using: .utf8)!)
    exit(1)
}

/// Renders the source into a transparent square canvas of @p pixels, scaled to the icon grid.
func render(pixels: Int) -> Data {
    let rep = NSBitmapImageRep(
        bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels, bitsPerSample: 8,
        samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
        bytesPerRow: 0, bitsPerPixel: 0)!
    rep.size = NSSize(width: pixels, height: pixels)
    let context = NSGraphicsContext(bitmapImageRep: rep)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = context
    context.imageInterpolation = .high
    let side = (Double(pixels) * artworkFraction).rounded()
    let origin = (Double(pixels) - side) / 2
    source.draw(
        in: NSRect(x: origin, y: origin, width: side, height: side), from: .zero,
        operation: .sourceOver, fraction: 1.0)
    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

let iconset = NSTemporaryDirectory() + "contour-logo-\(ProcessInfo.processInfo.processIdentifier).iconset"
try FileManager.default.createDirectory(atPath: iconset, withIntermediateDirectories: true)

for member in members {
    let suffix = member.scale == 1 ? "" : "@\(member.scale)x"
    let name = "icon_\(member.points)x\(member.points)\(suffix).png"
    try render(pixels: member.points * member.scale).write(to: URL(fileURLWithPath: "\(iconset)/\(name)"))
}

let iconutil = Process()
iconutil.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
iconutil.arguments = ["-c", "icns", iconset, "-o", outputPath]
try iconutil.run()
iconutil.waitUntilExit()
// exit() skips deferred work, so the scratch iconset is removed explicitly.
try? FileManager.default.removeItem(atPath: iconset)
exit(iconutil.terminationStatus)
