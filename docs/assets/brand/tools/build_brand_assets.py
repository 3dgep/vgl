"""vgl brand asset generator.

Masters (Illustrator exports, never edited by this script):
  svg/vgl-icon.svg    64x64 app icon
  svg/vgl-banner.svg  lockup: globe + "vgl" wordmark + subtitle on Orbital Slate
Every other logo, wordmark, social image and favicon is derived from their
geometry. Only the social-card tagline is outlined here (Inter, google/fonts).
Usage: python3 build_brand_assets.py <asset_root>   (needs fonts/Inter.ttf)
"""
import os, re, sys, json, copy
import xml.etree.ElementTree as ET
import resvg_py
from PIL import Image
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.transformPen import TransformPen
from fontTools.varLib.instancer import instantiateVariableFont

OUT = sys.argv[1] if len(sys.argv) > 1 else "vgl-brand-assets"
FONT_DIR = os.environ.get("VGL_FONTS", "fonts")
NS = "{http://www.w3.org/2000/svg}"

C = dict(orbital="#0B132B", ocean="#0F1D40", blue="#0284C7", cyan="#38BDF8",
         emerald="#10B981", amber="#F59E0B", starlight="#F8FAFC", geoid="#94A3B8",
         slate600="#475569")
THEMES = {  # wordmark stroke, subtitle, g dot
    "dark":  dict(text=C["starlight"], muted=C["geoid"],    dot=C["cyan"]),
    "light": dict(text=C["orbital"],   muted=C["slate600"], dot=C["blue"]),
}

# ---------------------------------------------------------------- master parsing
def css_classes(root):
    rules = {}
    style = root.find(f".//{NS}style")
    if style is None: return rules
    for sel, body in re.findall(r"([^{}]+)\{([^}]*)\}", style.text):
        decl = dict((k.strip(), v.strip()) for k, v in
                    (d.split(":", 1) for d in body.split(";") if ":" in d))
        for s in sel.split(","):
            rules.setdefault(s.strip().lstrip("."), {}).update(decl)
    return rules

def inline(el, rules):
    """Serialize an element tree with CSS classes resolved to attributes."""
    el = copy.deepcopy(el)
    for e in el.iter():
        cls = e.attrib.pop("class", None)
        e.attrib.pop("id", None)
        for c in (cls or "").split():
            for k, v in rules.get(c, {}).items():
                e.set(k, re.sub(r"px$", "", v))
        # ry duplicates rx on Illustrator rects; keep it, it's harmless
    s = ET.tostring(el, encoding="unicode")
    return re.sub(r"\s?xmlns(:\w+)?=\"[^\"]+\"", "", s).replace("ns0:", "")

def load(path):
    root = ET.parse(path).getroot()
    return root, css_classes(root)

banner_root, B_RULES = load(f"{OUT}/svg/vgl-banner.svg")
BW, BH = map(float, banner_root.get("viewBox").split()[2:])
kids = [k for k in banner_root if k.tag != f"{NS}defs"]
b_bg, b_text, b_globe, b_cut, b_tiles = kids
b_strokes, b_dot, b_sub = list(b_text)

icon_root, I_RULES = load(f"{OUT}/svg/vgl-icon.svg")
ikids = [k for k in icon_root if k.tag != f"{NS}defs"]
i_bg, i_globe, i_cut, i_tiles = ikids

def themed_text(theme, subtitle=True):
    t = THEMES[theme]
    s = inline(b_strokes, B_RULES).replace("#f8fafc", t["text"])
    d = inline(b_dot, B_RULES).replace("#38bdf8", t["dot"])
    sub = inline(b_sub, B_RULES).replace("#94a3b8", t["muted"]) if subtitle else ""
    return f"<g>{s}{d}{sub}</g>"

def knockout(globe_el, cut_el, tiles_el, rules, uid):
    """Globe with the quadtree keyline cut out (transparent) + tiles."""
    x, y, w, h = (float(cut_el.get(a)) for a in ("x", "y", "width", "height"))
    r = float(cut_el.get("rx", 0))
    n = lambda v: f"{round(v, 3):g}"
    x, y, w, h, r = (round(v, 3) for v in (x, y, w, h, r))
    rr = (f"M{n(x+r)} {n(y)}H{n(x+w-r)}A{n(r)} {n(r)} 0 0 1 {n(x+w)} {n(y+r)}V{n(y+h-r)}"
          f"A{n(r)} {n(r)} 0 0 1 {n(x+w-r)} {n(y+h)}H{n(x+r)}A{n(r)} {n(r)} 0 0 1 {n(x)} {n(y+h-r)}"
          f"V{n(y+r)}A{n(r)} {n(r)} 0 0 1 {n(x+r)} {n(y)}Z")
    clip = (f'<clipPath id="{uid}"><path clip-rule="evenodd" '
            f'd="M-1000-1000H2000V2000H-1000Z{rr}"/></clipPath>')
    return (f'<defs>{clip}</defs><g clip-path="url(#{uid})">{inline(globe_el, rules)}</g>'
            + inline(tiles_el, rules))

def banner_globe(uid): return knockout(b_globe, b_cut, b_tiles, B_RULES, uid)

# ---------------------------------------------------------------- helpers
def svg(w, h, body, title="vgl: Virtual Globe Library"):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w:.2f} {h:.2f}" '
            f'width="{w:.2f}" height="{h:.2f}" role="img" aria-label="{title}">'
            f'<title>{title}</title>{body}</svg>\n')

def ink_bbox(body, w=BW, h=BH, scale=8):
    """Ink bounds of `body` (in w x h user space) by rasterizing it."""
    png = resvg_py.svg_to_bytes(svg_string=svg(w, h, body), width=int(w * scale))
    import io
    im = Image.open(io.BytesIO(bytes(png))).getchannel("A")
    x0, y0, x1, y1 = im.point(lambda a: 255 if a > 8 else 0).getbbox()
    return x0 / scale, y0 / scale, x1 / scale, y1 / scale

def write(path, s):
    os.makedirs(os.path.dirname(f"{OUT}/{path}"), exist_ok=True)
    with open(f"{OUT}/{path}", "w") as f: f.write(s)

def place(body, box, pad):
    """Translate body so `box` sits at (pad, pad); return body, W, H."""
    x0, y0, x1, y1 = box
    return (f'<g transform="translate({pad - x0:.3f} {pad - y0:.3f})">{body}</g>',
            x1 - x0 + 2 * pad, y1 - y0 + 2 * pad)

# measured once, on the dark theme (light is identical geometry)
TEXT_BOX = ink_bbox(themed_text("dark"))
WORD_BOX = ink_bbox(themed_text("dark", subtitle=False))
GLOBE_BOX = ink_bbox(banner_globe("m0"))
LOCKUP_BOX = (min(TEXT_BOX[0], GLOBE_BOX[0]), min(TEXT_BOX[1], GLOBE_BOX[1]),
              max(TEXT_BOX[2], GLOBE_BOX[2]), max(TEXT_BOX[3], GLOBE_BOX[3]))
PAD = 8

def lockup(theme, uid):
    return banner_globe(uid) + themed_text(theme)

# ---------------------------------------------------------------- logos
for th in THEMES:
    body, W, H = place(lockup(th, f"h{th}"), LOCKUP_BOX, PAD)
    write(f"svg/vgl-logo-{th}.svg", svg(W, H, body))

    body, W, H = place(themed_text(th, subtitle=False), WORD_BOX, 0)
    write(f"svg/vgl-wordmark-{th}.svg", svg(W, H, body, "vgl"))

    # stacked: banner globe centred above the banner text block, same scale
    gx0, gy0, gx1, gy1 = GLOBE_BOX; tx0, ty0, tx1, ty1 = TEXT_BOX
    width = max(gx1 - gx0, tx1 - tx0); gap = 26
    g = f'<g transform="translate({(width-(gx1-gx0))/2 - gx0:.3f} {-gy0:.3f})">{banner_globe(f"s{th}")}</g>'
    t = (f'<g transform="translate({(width-(tx1-tx0))/2 - tx0:.3f} {gy1-gy0+gap-ty0:.3f})">'
         f'{themed_text(th)}</g>')
    body, W, H = place(g + t, (0, 0, width, gy1 - gy0 + gap + ty1 - ty0), PAD)
    write(f"svg/vgl-logo-stacked-{th}.svg", svg(W, H, body))

# knockout mark from the 64 icon
write("svg/vgl-mark.svg", svg(64, 64, knockout(i_globe, i_cut, i_tiles, I_RULES, "mk"), "vgl"))

# ---------------------------------------------------------------- social cards
def load_font(path, **loc):
    f = TTFont(path)
    if "fvar" in f:
        axes = {a.axisTag for a in f["fvar"].axes}
        f = instantiateVariableFont(f, {k: v for k, v in loc.items() if k in axes})
    return f

INTER = load_font(f"{FONT_DIR}/Inter.ttf", wght=400, opsz=24)
INTER_MED = load_font(f"{FONT_DIR}/Inter.ttf", wght=500, opsz=14)

def outline(font, text, size):
    gs = font.getGlyphSet(); cmap = font.getBestCmap(); s = size / font["head"].unitsPerEm
    x = 0.0; parts = []; xmin = xmax = None
    for ch in text:
        g = gs[cmap[ord(ch)]]
        sp = SVGPathPen(gs, ntos=lambda v: f"{v:.2f}".rstrip("0").rstrip("."))
        g.draw(TransformPen(sp, (s, 0, 0, -s, x, 0)))
        if sp.getCommands(): parts.append(sp.getCommands())
        bp = BoundsPen(gs); g.draw(TransformPen(bp, (s, 0, 0, -s, x, 0)))
        if bp.bounds:
            xmin = bp.bounds[0] if xmin is None else min(xmin, bp.bounds[0])
            xmax = bp.bounds[2] if xmax is None else max(xmax, bp.bounds[2])
        x += g.width * s
    return " ".join(parts), xmin, xmax

def social(W, H, uid, lockup_w):
    grid = 40
    lines = "".join(f"M{x} 0V{H}" for x in range(grid, W, grid)) + \
            "".join(f"M0 {y}H{W}" for y in range(grid, H, grid))
    body = (f'<rect width="{W}" height="{H}" fill="{C["orbital"]}"/>'
            f'<path d="{lines}" stroke="{C["ocean"]}" stroke-width="1.5"/>')
    x0, y0, x1, y1 = LOCKUP_BOX
    s = lockup_w / (x1 - x0); lh = (y1 - y0) * s
    lx = (W - lockup_w) / 2 - x0 * s; ly = H * 0.42 - lh / 2 - y0 * s
    body += f'<g transform="translate({lx:.2f} {ly:.2f}) scale({s:.4f})">{lockup("dark", uid)}</g>'
    d, a, b = outline(INTER, "Level-of-detail terrain and imagery on the WGS84 ellipsoid", 32 if W >= 1280 else 31)
    body += (f'<path transform="translate({(W-(b-a))/2 - a:.2f} {H*0.42 + lh/2 + 70:.2f})" '
             f'fill="{C["geoid"]}" d="{d}"/>')
    d, a, b = outline(INTER_MED, "github.com/3dgep/vgl", 22)
    body += f'<path transform="translate({(W-(b-a))/2 - a:.2f} {H-56})" fill="{C["cyan"]}" d="{d}"/>'
    return svg(W, H, body)

write("svg/vgl-social-preview.svg", social(1280, 640, "sp", 680))
write("svg/vgl-og-image.svg", social(1200, 630, "og", 650))

# ---------------------------------------------------------------- icon-derived favicons
def full_bleed(size, frac):
    inner = "".join(inline(e, I_RULES) for e in (i_globe, i_cut, i_tiles))
    x0, y0, x1, y1 = ink_bbox(inner, 64, 64)
    s = size * frac / max(x1 - x0, y1 - y0)
    tx = (size - (x1 - x0) * s) / 2 - x0 * s; ty = (size - (y1 - y0) * s) / 2 - y0 * s
    return svg(size, size, f'<rect width="{size}" height="{size}" fill="{C["orbital"]}"/>'
               f'<g transform="translate({tx:.2f} {ty:.2f}) scale({s:.4f})">{inner}</g>', "vgl")

ICON16 = (f'<rect width="16" height="16" rx="3.5" fill="{C["orbital"]}"/>'
          f'<circle cx="7.5" cy="8.5" r="5.75" fill="{C["ocean"]}"/>'
          f'<path d="M7.5 3V14M2 8.5H13M4 5.5H11M4 11.5H11" stroke="{C["blue"]}" stroke-width="1" shape-rendering="crispEdges"/>'
          f'<circle cx="7.5" cy="8.5" r="5.75" fill="none" stroke="{C["cyan"]}" stroke-width="1.5"/>'
          f'<rect x="7" y="0" width="9" height="9" fill="{C["orbital"]}" shape-rendering="crispEdges"/>'
          + "".join(f'<rect x="{x}" y="{y}" width="3" height="3" fill="{f}" shape-rendering="crispEdges"/>'
                    for x, y, f in [(8, 1, C["cyan"]), (12, 1, C["amber"]), (8, 5, C["emerald"]), (12, 5, C["cyan"])]))
write("favicon/favicon-16.svg", svg(16, 16, ICON16, "vgl"))
with open(f"{OUT}/svg/vgl-icon.svg") as f: write("favicon/favicon.svg", f.read())
write("favicon/site.webmanifest", json.dumps({
    "name": "vgl: Virtual Globe Library", "short_name": "vgl",
    "icons": [{"src": "icon-192.png", "sizes": "192x192", "type": "image/png"},
              {"src": "icon-512.png", "sizes": "512x512", "type": "image/png"},
              {"src": "icon-maskable-512.png", "sizes": "512x512", "type": "image/png", "purpose": "maskable"}],
    "theme_color": C["orbital"], "background_color": C["orbital"], "display": "standalone"}, indent=2) + "\n")

# ---------------------------------------------------------------- rasters
def png(src, w, dst=None, svg_string=None):
    b = bytes(resvg_py.svg_to_bytes(svg_string=svg_string, width=w) if svg_string
              else resvg_py.svg_to_bytes(svg_path=f"{OUT}/{src}", width=w))
    os.makedirs(os.path.dirname(f"{OUT}/{dst}"), exist_ok=True)
    open(f"{OUT}/{dst}", "wb").write(b)

png("favicon/favicon-16.svg", 16, "favicon/favicon-16.png")
for s in (32, 48): png("svg/vgl-icon.svg", s, f"favicon/favicon-{s}.png")
for s in (192, 512): png("svg/vgl-icon.svg", s, f"favicon/icon-{s}.png")
png(None, 180, "favicon/apple-touch-icon.png", svg_string=full_bleed(180, 0.74))
png(None, 512, "favicon/icon-maskable-512.png", svg_string=full_bleed(512, 0.62))
ims = [Image.open(f"{OUT}/favicon/favicon-{s}.png").convert("RGBA") for s in (16, 32, 48)]
ims[2].save(f"{OUT}/favicon/favicon.ico", format="ICO", sizes=[(16, 16), (32, 32), (48, 48)],
            append_images=ims[:2])

for s in (64, 128, 256, 512, 1024): png("svg/vgl-icon.svg", s, f"png/vgl-icon-{s}.png")
for s in (256, 512): png("svg/vgl-mark.svg", s, f"png/vgl-mark-{s}.png")
for th in THEMES:
    for sfx, w in (("", 480), ("@2x", 960)):
        png(f"svg/vgl-logo-{th}.svg", w, f"png/vgl-logo-{th}{sfx}.png")
        png(f"svg/vgl-logo-stacked-{th}.svg", w // 2, f"png/vgl-logo-stacked-{th}{sfx}.png")
        png(f"svg/vgl-wordmark-{th}.svg", w // 2, f"png/vgl-wordmark-{th}{sfx}.png")
png("svg/vgl-banner.svg", 960, "png/vgl-banner.png")
png("svg/vgl-social-preview.svg", 1280, "png/vgl-social-preview.png")
png("svg/vgl-og-image.svg", 1200, "png/vgl-og-image.png")
for f in ("png/vgl-social-preview.png", "png/vgl-og-image.png"):
    Image.open(f"{OUT}/{f}").convert("RGB").save(f"{OUT}/{f}", optimize=True)
print("done", TEXT_BOX, GLOBE_BOX)
