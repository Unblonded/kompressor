"""Renders the Kompressor app icon (glossy graphite tile, metal zipper, violet accent).

Usage:  python tools/make_icon.py [out_dir]
Writes  icon_1024.png and a .ico with every Windows size into out_dir (default: packaging/icons).
Only needs Pillow. Everything is drawn at 2x and downsampled for anti-aliasing.
"""
import lzma
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

SS = 2                    # supersampling factor
N = 1024 * SS             # working canvas
ACCENT = (140, 109, 255)  # app violet
ACCENT_DARK = (86, 58, 214)
ACCENT_LIGHT = (196, 182, 255)


def s(v):
    return int(round(v * SS))


def vgrad(w, h, top, bottom):
    """Vertical RGBA gradient image."""
    mask = Image.linear_gradient("L").resize((w, h))
    a = Image.new("RGBA", (w, h), top)
    b = Image.new("RGBA", (w, h), bottom)
    return Image.composite(b, a, mask)


def hgrad(w, h, left, right):
    return vgrad(h, w, left, right).rotate(90, expand=True).transpose(Image.FLIP_TOP_BOTTOM)


def rrect_mask(size, box, radius):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).rounded_rectangle(box, radius=radius, fill=255)
    return m


def paste_masked(dst, src, mask, pos=(0, 0)):
    layer = Image.new("RGBA", dst.size, (0, 0, 0, 0))
    layer.paste(src, pos)
    full = Image.new("L", dst.size, 0)
    full.paste(mask, pos)
    a = ImageChops.multiply(layer.getchannel("A"), full)
    layer.putalpha(a)
    dst.alpha_composite(layer)


def with_alpha(img, alpha):
    img = img.copy()
    img.putalpha(ImageChops.multiply(img.getchannel("A"), Image.new("L", img.size, int(alpha * 255))))
    return img


def tooth(w, h, flip):
    """One metal zipper tooth: rounded block with a metallic gradient and a dark seam."""
    t = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    body = vgrad(w, h, (236, 238, 245, 255), (120, 125, 140, 255))
    shine = hgrad(w, h, (255, 255, 255, 0), (255, 255, 255, 90)) if flip else hgrad(w, h, (255, 255, 255, 90), (255, 255, 255, 0))
    body.alpha_composite(shine)
    m = rrect_mask((w, h), (0, 0, w - 1, h - 1), radius=int(h * 0.42))
    paste_masked(t, body, m)
    d = ImageDraw.Draw(t)
    d.rounded_rectangle((0, 0, w - 1, h - 1), radius=int(h * 0.42), outline=(28, 30, 38, 230), width=max(2, s(3)))
    # cup where the opposite tooth locks in
    cx = w - int(w * 0.22) if not flip else int(w * 0.22)
    d.ellipse((cx - int(h * 0.16), h // 2 - int(h * 0.16), cx + int(h * 0.16), h // 2 + int(h * 0.16)), fill=(70, 74, 88, 170))
    return t


def render():
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))

    # ---- tile: drop shadow, graphite body, rim light, gloss
    inset, radius = s(76), s(196)
    box = (inset, inset, N - inset, N - inset)
    shadow = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((box[0], box[1] + s(26), box[2], box[3] + s(26)), radius=radius, fill=(0, 0, 0, 150))
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(s(34))))

    tile_mask = rrect_mask((N, N), box, radius)
    body = vgrad(N, N, (74, 78, 92, 255), (18, 19, 25, 255))
    paste_masked(img, body, tile_mask)

    # subtle violet bloom low in the tile (the accent "light" escaping from the open zipper)
    bloom = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    ImageDraw.Draw(bloom).ellipse((s(300), s(640), s(724), s(1010)), fill=ACCENT + (120,))
    bloom = bloom.filter(ImageFilter.GaussianBlur(s(90)))
    paste_masked(img, bloom, tile_mask)

    # rim: bright on top, fading down
    rim = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    ImageDraw.Draw(rim).rounded_rectangle(box, radius=radius, outline=(255, 255, 255, 255), width=s(5))
    fade = Image.linear_gradient("L").resize((N, N)).point(lambda v: int((255 - v) * 0.30))
    rim.putalpha(ImageChops.multiply(rim.getchannel("A"), fade))
    img.alpha_composite(rim)
    edge = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    ImageDraw.Draw(edge).rounded_rectangle(box, radius=radius, outline=(0, 0, 0, 140), width=s(3))
    img.alpha_composite(edge)

    # ---- zipper
    cx = N // 2
    tw, th = s(112), s(44)          # tooth size
    pitch = s(52)                   # vertical distance between two teeth on the same side
    lock = s(22)                    # how far each tooth crosses the centre line

    def spread(y):                  # how far apart the two rows are at height y (V below the slider)
        return 0 if y < slider_y + s(60) else int((y - slider_y - s(60)) * 0.42)
    top_y, slider_y, bottom_y = s(150), s(500), s(880)

    # fabric tapes either side of the teeth, splitting apart below the slider
    tape_w = s(150)
    tapes = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    td = ImageDraw.Draw(tapes)
    ys = list(range(top_y - s(60), bottom_y + s(120), s(8)))
    for side in (-1, 1):
        pts = [(cx + side * (lock + s(40) + spread(y)), y) for y in ys]
        td.line(pts, fill=(52, 55, 68, 255), width=tape_w, joint="curve")
    tape_shadow = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    tape_shadow.paste((0, 0, 0, 160), (0, 0), tapes.getchannel("A"))
    paste_masked(img, tape_shadow.filter(ImageFilter.GaussianBlur(s(14))), tile_mask)
    tape_fill = vgrad(N, N, (66, 70, 86, 255), (34, 36, 46, 255))
    paste_masked(img, tape_fill, ImageChops.multiply(tapes.getchannel("A"), tile_mask))
    stitch = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    sd = ImageDraw.Draw(stitch)
    for side in (-1, 1):
        for k in range(0, len(ys) - 3, 4):
            y0, y1 = ys[k], ys[k + 2]
            x0 = cx + side * (lock + s(40) + spread(y0) + tape_w // 2 - s(22))
            x1 = cx + side * (lock + s(40) + spread(y1) + tape_w // 2 - s(22))
            sd.line([(x0, y0), (x1, y1)], fill=(150, 155, 172, 150), width=s(4))
    paste_masked(img, stitch, tile_mask)

    # violet glow inside the open part of the zipper (behind the separated teeth)
    glow = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.polygon([(cx - s(20), slider_y + s(70)), (cx + s(20), slider_y + s(70)), (cx + s(190), bottom_y + s(40)),
                (cx - s(190), bottom_y + s(40))], fill=ACCENT + (255,))
    soft = glow.filter(ImageFilter.GaussianBlur(s(40)))
    paste_masked(img, soft, tile_mask)
    core = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    ImageDraw.Draw(core).polygon([(cx - s(6), slider_y + s(80)), (cx + s(6), slider_y + s(80)), (cx + s(120), bottom_y + s(40)),
                                  (cx - s(120), bottom_y + s(40))], fill=ACCENT_LIGHT + (255,))
    paste_masked(img, core.filter(ImageFilter.GaussianBlur(s(14))), tile_mask)

    left, right = tooth(tw, th, False), tooth(tw, th, True)
    teeth = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    y, i = top_y, 0
    while y < bottom_y - th // 2:
        sp = spread(y)
        if i % 2 == 0:
            teeth.alpha_composite(left, (cx + lock - tw - sp, y))
        else:
            teeth.alpha_composite(right, (cx - lock + sp, y))
        y += pitch // 2 + s(2)
        i += 1
    # teeth drop shadow
    tsh = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    tsh.paste((0, 0, 0, 170), (0, 0), teeth.getchannel("A"))
    img.alpha_composite(tsh.filter(ImageFilter.GaussianBlur(s(8))), (0, s(6)))
    paste_masked(img, teeth, tile_mask)

    # ---- slider (metal body)
    sw, sh = s(250), s(170)
    sx, sy = cx - sw // 2, slider_y - s(40)
    slider_shadow = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    ImageDraw.Draw(slider_shadow).rounded_rectangle((sx, sy + s(16), sx + sw, sy + sh + s(16)), radius=s(62), fill=(0, 0, 0, 170))
    img.alpha_composite(slider_shadow.filter(ImageFilter.GaussianBlur(s(18))))
    smask = Image.new("L", (sw, sh), 0)
    ImageDraw.Draw(smask).polygon([(0, s(20)), (sw, s(20)), (sw - s(52), sh), (s(52), sh)], fill=255)
    smask = Image.composite(smask, Image.new("L", (sw, sh), 0), rrect_mask((sw, sh), (0, 0, sw - 1, sh - 1), s(56)))
    smask = smask.filter(ImageFilter.GaussianBlur(s(2)))
    sbody = vgrad(sw, sh, (252, 252, 255, 255), (112, 117, 134, 255))
    sbody.alpha_composite(hgrad(sw, sh, (255, 255, 255, 0), (0, 0, 0, 40)))
    paste_masked(img, sbody, smask, (sx, sy))
    srim = Image.new("RGBA", (sw, sh), (0, 0, 0, 0))
    ImageDraw.Draw(srim).polygon([(0, s(20)), (sw, s(20)), (sw - s(52), sh), (s(52), sh)], outline=(40, 42, 52, 210), width=s(4))
    paste_masked(img, srim, rrect_mask((sw, sh), (0, 0, sw - 1, sh - 1), s(56)), (sx, sy))
    # gloss stripe on the slider
    gl = vgrad(sw, s(46), (255, 255, 255, 170), (255, 255, 255, 0))
    paste_masked(img, gl, rrect_mask((sw - s(60), s(46)), (0, 0, sw - s(61), s(45)), s(22)).resize((sw, s(46))), (sx, sy + s(26)))

    # ---- pull tab (the accent)
    pw, ph = s(132), s(300)
    px, py = cx - pw // 2, sy + sh - s(60)
    tab_shadow = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    ImageDraw.Draw(tab_shadow).rounded_rectangle((px + s(10), py + s(22), px + pw + s(10), py + ph + s(22)), radius=s(60), fill=(0, 0, 0, 190))
    img.alpha_composite(tab_shadow.filter(ImageFilter.GaussianBlur(s(20))))
    tmask = rrect_mask((pw, ph), (0, 0, pw - 1, ph - 1), s(62))
    hole = Image.new("L", (pw, ph), 0)
    ImageDraw.Draw(hole).rounded_rectangle((s(36), ph - s(118), pw - s(36), ph - s(42)), radius=s(30), fill=255)
    tmask = ImageChops.subtract(tmask, hole)
    tbody = vgrad(pw, ph, ACCENT_LIGHT + (255,), ACCENT_DARK + (255,))
    tbody.alpha_composite(hgrad(pw, ph, (255, 255, 255, 70), (0, 0, 0, 60)))
    paste_masked(img, tbody, tmask, (px, py))
    trim = Image.new("RGBA", (pw, ph), (0, 0, 0, 0))
    ImageDraw.Draw(trim).rounded_rectangle((0, 0, pw - 1, ph - 1), radius=s(62), outline=(255, 255, 255, 120), width=s(4))
    ImageDraw.Draw(trim).rounded_rectangle((s(36), ph - s(118), pw - s(36), ph - s(42)), radius=s(30), outline=(40, 24, 110, 200), width=s(4))
    paste_masked(img, trim, rrect_mask((pw, ph), (0, 0, pw - 1, ph - 1), s(62)), (px, py))
    tgl = vgrad(pw - s(40), s(120), (255, 255, 255, 150), (255, 255, 255, 0))
    paste_masked(img, tgl, rrect_mask((pw - s(40), s(120)), (0, 0, pw - s(41), s(119)), s(40)), (px + s(20), py + s(16)))
    # rivet joining tab and slider
    rv = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    ImageDraw.Draw(rv).ellipse((cx - s(30), py - s(4), cx + s(30), py + s(56)), fill=(230, 232, 240, 255), outline=(40, 42, 52, 220), width=s(4))
    img.alpha_composite(rv)

    # ---- top-half gloss over the whole tile
    gloss = vgrad(N, N // 2, (255, 255, 255, 46), (255, 255, 255, 0))
    gmask = Image.new("L", (N, N), 0)
    ImageDraw.Draw(gmask).ellipse((-s(300), -s(560), N + s(300), N // 2 - s(20)), fill=255)
    gmask = ImageChops.multiply(gmask, tile_mask).crop((0, 0, N, N // 2))
    paste_masked(img, gloss, gmask, (0, 0))

    return img.resize((1024, 1024), Image.LANCZOS)


def small(icon, size):
    """Downscale with a touch of sharpening so the zipper still reads at 16-32 px."""
    im = icon.resize((size, size), Image.LANCZOS)
    if size <= 48:
        im = im.filter(ImageFilter.UnsharpMask(radius=0.6, percent=60, threshold=0))
    return im


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "packaging", "icons")
    os.makedirs(out, exist_ok=True)
    icon = render()
    icon.save(os.path.join(out, "icon_1024.png"))
    for sz in (512, 256, 128, 64, 32, 16):
        small(icon, sz).save(os.path.join(out, f"icon_{sz}.png"))
    sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
    frames = [small(icon, sz) for sz in sizes]
    frames[-1].save(os.path.join(out, "kompressor.ico"), format="ICO", sizes=[(sz, sz) for sz in sizes], append_images=frames[:-1])
    print("wrote", os.path.abspath(out))
    if len(sys.argv) <= 1:  # default run also refreshes the copy compiled into the app
        write_embed(icon, os.path.join(os.path.dirname(__file__), "..", "src", "gui", "app_icon_data.inc"))


EMBED_SIZES = (32, 64, 128, 256)


def write_embed(icon, path):
    """Embed straight-alpha RGBA at a few sizes, xz-compressed (liblzma is already linked)."""
    raw = b"".join(small(icon, sz).convert("RGBA").tobytes() for sz in EMBED_SIZES)
    blob = lzma.compress(raw, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32, preset=9 | lzma.PRESET_EXTREME)
    lines = ["// Generated by tools/make_icon.py - do not edit.",
             f"// RGBA images of {', '.join(str(s) for s in EMBED_SIZES)} px, concatenated and xz-compressed.",
             f"static const int kIconSizes[] = {{{', '.join(str(s) for s in EMBED_SIZES)}}};",
             f"static const unsigned long kIconRawSize = {len(raw)}ul;",
             f"static const unsigned char kIconXz[{len(blob)}] = {{"]
    for i in range(0, len(blob), 24):
        lines.append("  " + ",".join(str(b) for b in blob[i:i + 24]) + ",")
    lines.append("};")
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", os.path.abspath(path), f"({len(blob)} bytes compressed)")


if __name__ == "__main__":
    main()
