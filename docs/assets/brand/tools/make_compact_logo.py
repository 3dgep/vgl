# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

"""Derives the compact logos from the full ones, for small sizes such as a site's nav bar.

    svg/vgl-logo-dark.svg           ->  svg/vgl-logo-compact-dark.svg
    svg/vgl-logo-light.svg          ->  svg/vgl-logo-compact-light.svg
    svg/vgl-logo-stacked-dark.svg   ->  svg/vgl-logo-stacked-compact-dark.svg
    svg/vgl-logo-stacked-light.svg  ->  svg/vgl-logo-stacked-compact-light.svg

The subtitle (VIRTUAL GLOBE LIBRARY), unreadable at small sizes, is removed, and the "vgl"
letters are scaled up until they fill the height the letters and the subtitle shared: beside
the globe, down to its lower edge; below it, down to where the subtitle ended, centred under
the globe. Gaps and margins stay as they are, and the canvas widens by what the letters gain.

With resvg-py installed, as build_brand_assets.py uses, it also renders them to png/ at that
script's sizes: the horizontal logos 480 and 960 px wide, the stacked ones 240 and 480 px. Run
from the kit's folder:

    python3 tools/make_compact_logo.py .
"""

import os
import re
import sys

# Ink of the "vgl" letters in the logos' coordinates (inside their own translate): the strokes'
# centre lines, plus half the 11.5 px stroke.
HALF = 11.5 / 2
LETTERS_LEFT = 176.72 - HALF
LETTERS_TOP = 36.75 - HALF  # The l's ascender.
LETTERS_RIGHT = 303.72 + HALF  # The l's foot.
LETTERS_BOTTOM = 83.75 + 27 + 18 + HALF  # The g's hook.
LETTERS_CENTRE = (LETTERS_LEFT + LETTERS_RIGHT) / 2
# The globe's lower edge in the horizontal logo: its ring (r=47.95 about y=104.49) and half its
# 7.92 px stroke.
GLOBE_BOTTOM = 104.49 + 47.95 + 7.92 / 2
# The stacked logo's margin around its content.
STACKED_MARGIN = 8.0

SUBTITLE = re.compile(r'\s*<path d="M177\.74,145\.5[^"]*" fill="#[0-9A-Fa-f]{6}" />')
NUMBER = r"(-?[\d.]+)"


def without_subtitle(text):
    text, count = SUBTITLE.subn("", text)
    if count != 1:
        raise ValueError("subtitle not found")
    return text


def scale_letters(text, start, end, cx, top, s):
    """Wraps text[start:end], the letters' group, in a scale by s about (cx, top)."""
    transform = f"translate({cx:.3f} {top:.3f}) scale({s:.5f}) translate({-cx:.3f} {-top:.3f})"
    return text[:start] + f'<g transform="{transform}">' + text[start:end] + "</g>" + text[end:]


def resize(text, old_w, old_h, new_w, label=True):
    text = text.replace(f'viewBox="0 0 {old_w} {old_h}"', f'viewBox="0 0 {new_w:.2f} {old_h}"', 1)
    text = text.replace(f'width="{old_w}"', f'width="{new_w:.2f}"', 1)
    if label:
        text = text.replace('aria-label="vgl: Virtual Globe Library"', 'aria-label="vgl"', 1)
        text = text.replace("<title>vgl: Virtual Globe Library</title>", "<title>vgl</title>", 1)
    return text


def compact_horizontal(text):
    view = re.search(rf'viewBox="0 0 {NUMBER} {NUMBER}"', text)
    shift = re.search(rf'<g transform="translate\({NUMBER} {NUMBER}\)">', text)
    width, dx = float(view.group(1)), float(shift.group(1))
    right_margin = (width - dx) - LETTERS_RIGHT
    text = without_subtitle(text)
    # The letters' group starts at the last "<g><g>" and ends with the logo.
    start, end = text.rindex("<g><g>"), text.rindex("</g></g></svg>")
    s = (GLOBE_BOTTOM - LETTERS_TOP) / (LETTERS_BOTTOM - LETTERS_TOP)
    text = scale_letters(text, start, end, LETTERS_LEFT, LETTERS_TOP, s)
    new_width = LETTERS_LEFT + (LETTERS_RIGHT - LETTERS_LEFT) * s + right_margin + dx
    return resize(text, view.group(1), view.group(2), new_width), s, new_width


def compact_stacked(text):
    view = re.search(rf'viewBox="0 0 {NUMBER} {NUMBER}"', text)
    outer = re.search(rf'<g transform="translate\({NUMBER} {NUMBER}\)">', text)
    letters = list(re.finditer(rf'<g transform="translate\({NUMBER} {NUMBER}\)"><g><g>', text))[-1]
    width, height = float(view.group(1)), float(view.group(2))
    letters_dy = float(letters.group(2))
    # Where the subtitle ended, in the letters' coordinates: the canvas less its margins.
    bottom = height - 2 * STACKED_MARGIN - letters_dy
    text = without_subtitle(text)
    start = letters.end() - len("<g><g>")
    end = text.rindex("</g></g></g></svg>")
    s = (bottom - LETTERS_TOP) / (LETTERS_BOTTOM - LETTERS_TOP)
    text = scale_letters(text, start, end, LETTERS_CENTRE, LETTERS_TOP, s)
    # The letters grow about their centre: widen the canvas by what they gain, and move the
    # content over by half of it, so that the globe and the letters stay centred.
    gain = (LETTERS_RIGHT - LETTERS_LEFT) * (s - 1)
    new_width = width + gain
    ox, oy = float(outer.group(1)), float(outer.group(2))
    text = text.replace(outer.group(0), f'<g transform="translate({ox + gain / 2:.3f} {oy:.3f})">', 1)
    return resize(text, view.group(1), view.group(2), new_width), s, new_width


def render(root, svg_name, width, png_name):
    import resvg_py
    data = bytes(resvg_py.svg_to_bytes(svg_path=os.path.join(root, "svg", svg_name), width=width))
    with open(os.path.join(root, "png", png_name), "wb") as f:
        f.write(data)


def main(root):
    for theme in ("dark", "light"):
        for kind, make, widths in (("", compact_horizontal, (480, 960)),
                                   ("stacked-", compact_stacked, (240, 480))):
            src = f"vgl-logo-{kind}{theme}.svg"
            dst = f"vgl-logo-{kind}compact-{theme}.svg"
            with open(os.path.join(root, "svg", src), encoding="utf-8") as f:
                text, s, w = make(f.read())
            with open(os.path.join(root, "svg", dst), "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            print(f"svg/{dst}: letters x{s:.3f}, {w:.2f} wide")
            try:
                for width, suffix in zip(widths, ("", "@2x")):
                    render(root, dst, width, dst.replace(".svg", f"{suffix}.png"))
                    print(f"  png/{dst.replace('.svg', f'{suffix}.png')}: {width} px wide")
            except ImportError:
                print("  resvg-py is not installed: no PNG files")


main(sys.argv[1] if len(sys.argv) > 1 else ".")
