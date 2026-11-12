// SPDX-License-Identifier: GPL-3.0-or-later
import CoreGraphics

/// MyTether's symbol: a USB-A plug. The head overlaps the body and merges with it, and the
/// collar and an L-shaped cord sit below with clean gaps, wide enough to read as separate
/// pieces at menu bar size. The app and tools/make-icon both draw from here, which keeps every
/// icon identical.
func usbGlyphPath(in rect: CGRect) -> CGPath {
    let (unit, height) = unitGlyph()
    // Fit the plug's height into 94% of the box, centered.
    let side = min(rect.width, rect.height), scale = 0.94 / height
    var place = CGAffineTransform(translationX: rect.midX - side / 2, y: rect.midY - side / 2)
        .scaledBy(x: side, y: side)
        .translatedBy(x: 0.5, y: 0.5)
        .scaledBy(x: scale, y: scale)
        .translatedBy(x: -0.5, y: -height / 2)
    return unit.copy(using: &place) ?? unit
}

private func rounded(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat, _ r: CGFloat) -> CGPath {
    CGPath(roundedRect: CGRect(x: x, y: y, width: w, height: h), cornerWidth: r, cornerHeight: r, transform: nil)
}

/// Laid out with y growing downward, 1 unit wide. Returns the shape and its height.
private func unitGlyph() -> (CGPath, CGFloat) {
    var y: CGFloat = 0
    // Stacks a part of the given height, then leaves the gap below it (a negative gap overlaps
    // the next part). Returns the part's top.
    func stack(_ height: CGFloat, gap: CGFloat) -> CGFloat {
        defer { y += height + gap }
        return y
    }
    let headTop = stack(0.44, gap: -0.015)
    let bodyTop = stack(0.38, gap: 0.03)
    let collarTop = stack(0.07, gap: 0.03)
    let cordTop = stack(0.32, gap: 0)

    let collar: CGFloat = 0.306
    // Two windows set low in the head.
    let windows = rounded(0.355, headTop + 0.24, 0.13, 0.08, 0.016)
        .union(rounded(0.515, headTop + 0.24, 0.13, 0.08, 0.016))
    let shape = rounded(0.285, headTop, 0.43, 0.44, 0.08).subtracting(windows)
        .union(rounded(0.23, bodyTop, 0.54, 0.38, 0.11))
        .union(rounded(0.5 - collar / 2, collarTop, collar, 0.07, 0.035))
        .union(cordPath(top: cordTop, height: 0.32))
    return (shape, y)
}

/// The cord drops straight down, then turns right around a wide rounded corner.
private func cordPath(top: CGFloat, height: CGFloat) -> CGPath {
    let width: CGFloat = 0.12, corner: CGFloat = 0.16, reach: CGFloat = 0.22
    let r = width / 2, x0: CGFloat = 0.5, x1 = x0 + reach
    let start = top + r, turn = top + height - r
    let line = CGMutablePath()
    line.move(to: CGPoint(x: x0, y: start))
    line.addLine(to: CGPoint(x: x0, y: turn - corner))
    line.addArc(tangent1End: CGPoint(x: x0, y: turn), tangent2End: CGPoint(x: x1, y: turn), radius: corner)
    line.addLine(to: CGPoint(x: x1, y: turn))
    // Merging drops a stroke's round ends, so each end gets its own circle.
    let ends = CGPath(ellipseIn: CGRect(x: x0 - r, y: start - r, width: width, height: width), transform: nil)
        .union(CGPath(ellipseIn: CGRect(x: x1 - r, y: turn - r, width: width, height: width), transform: nil))
    return line.copy(strokingWithWidth: width, lineCap: .butt, lineJoin: .round, miterLimit: 1).union(ends)
}
