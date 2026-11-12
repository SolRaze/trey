// Renders the HOME Menu icon (48x48) and banner (256x128) in the mono theme: matte near-black field,
// white wireless mark like the 3DS wireless LED's (dot on a mast, two arcs each side), banner adds the word Relay.
//
//   swift art.swift        writes app/resources/icon.png and banner.png
//
// HOME Menu caches icons per title version: bump VERSION_REVISION in the Makefile after a change.
import AppKit

let field = CGColor(srgbRed: 12 / 255, green: 12 / 255, blue: 14 / 255, alpha: 1)    // mono field
let ink = CGColor(srgbRed: 236 / 255, green: 236 / 255, blue: 238 / 255, alpha: 1)   // mono white
let out = URL(fileURLWithPath: CommandLine.arguments[0]).deletingLastPathComponent()
    .appendingPathComponent("app/resources")

func canvas(_ w: Int, _ h: Int, _ draw: (CGContext) -> Void) -> CGImage {
    let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.setFillColor(field)
    ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
    draw(ctx)
    return ctx.makeImage()!
}

// Mark centred at c, sized by r = outer arc radius. Arcs span 42 degrees either side of horizontal.
func mark(_ ctx: CGContext, _ c: CGPoint, _ r: CGFloat) {
    ctx.setFillColor(ink)
    let dot = r * 0.26
    ctx.fillEllipse(in: CGRect(x: c.x - dot, y: c.y - dot, width: dot * 2, height: dot * 2))
    ctx.setStrokeColor(ink)
    ctx.setLineWidth(r * 0.2)
    ctx.setLineCap(.round)
    ctx.move(to: CGPoint(x: c.x, y: c.y))   // mast, dot down to the outer arcs' lower ends
    ctx.addLine(to: CGPoint(x: c.x, y: c.y - r * 0.95))
    ctx.strokePath()
    let span = 42 * CGFloat.pi / 180
    for radius in [r * 0.6, r] {
        for side in [0, CGFloat.pi] {
            ctx.addArc(center: c, radius: radius, startAngle: side - span, endAngle: side + span, clockwise: false)
            ctx.strokePath()
        }
    }
}

func save(_ img: CGImage, _ name: String) {
    let rep = NSBitmapImageRep(cgImage: img)
    try! rep.representation(using: .png, properties: [:])!.write(to: out.appendingPathComponent(name))
    print(out.appendingPathComponent(name).path)
}

save(canvas(48, 48) { mark($0, CGPoint(x: 24, y: 24), 17) }, "icon.png")

// Mark and word centred as one group; r + stroke half-width is the mark's reach either side.
save(canvas(256, 128) { ctx in
    let r: CGFloat = 34, reach = r * 1.1, gap: CGFloat = 16
    let font = NSFont.systemFont(ofSize: 40, weight: .medium)
    let word = NSAttributedString(string: "Relay", attributes: [.font: font, .foregroundColor: NSColor(cgColor: ink)!])
    let size = word.size()
    let left = (256 - (reach * 2 + gap + size.width)) / 2
    mark(ctx, CGPoint(x: left + reach, y: 64), r)
    NSGraphicsContext.current = NSGraphicsContext(cgContext: ctx, flipped: false)
    word.draw(at: CGPoint(x: left + reach * 2 + gap, y: 64 - size.height / 2 + font.descender / 2))
}, "banner.png")
