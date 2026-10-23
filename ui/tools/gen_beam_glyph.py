#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
# The generated Beam font and glyph data are licensed under OFL-1.1.
# This generator remains GPL-3.0-or-later.
"""The Beam glyph (U+F8D0): a small speaker box with two outgoing waves. It is drawn here on a 24-unit grid
and written as a one-glyph TrueType font that lv_font_conv merges into
font_icons_20.c and font_icons_28.c next to the Font Awesome icons:
    python3 tools/gen_beam_glyph.py beam.ttf
    lv_font_conv --font FontAwesome.ttf --size N --bpp 4 --format lvgl --range <the Font Awesome list> \\
                 --font beam.ttf --range 0xF8D0 --no-compress --force-fast-kern-format -o font_icons_N.c
The em is 24 units, so a glyph of size N px has a 24-unit grid of N px."""
import math
import sys
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

UNIT = 40            # font units per grid unit (em = 24 grid units)
STROKE = 2.0         # grid units; the mockup draws 1.8, a touch more ink survives the 20 px size
DX = 2.25            # the ink spans x 1..18.5 of the 24-unit box: shift it so the glyph is centred in its label


def P(x, y):         # grid (y down) -> font units (y up, the 24-unit box sits on the baseline)
    return (round((x + DX) * UNIT), round((24 - y) * UNIT))


def circle(cx, cy, r, n=24):                       # clockwise in font space: outer contour
    return [P(cx + r * math.cos(-2 * math.pi * i / n), cy + r * math.sin(-2 * math.pi * i / n)) for i in range(n)]


def rrect(x1, y1, x2, y2, r, cw, n=6):             # rounded rectangle, corners as short polylines
    pts = []
    for cx, cy, a0 in ((x2 - r, y1 + r, -90), (x2 - r, y2 - r, 0), (x1 + r, y2 - r, 90), (x1 + r, y1 + r, 180)):
        for i in range(n + 1):
            a = math.radians(a0 + 90 * i / n)
            pts.append(P(cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts if cw else pts[::-1]


def wave(x_end, y_a, y_b, radius, n=18):           # a stroked arc bulging right, round caps; the chord ends at x_end
    h = (y_b - y_a) / 2
    cx, cy = x_end - math.sqrt(radius * radius - h * h), (y_a + y_b) / 2
    a0 = math.asin(-h / radius); a1 = math.asin(h / radius)
    ro, ri = radius + STROKE / 2, radius - STROKE / 2
    outer = [P(cx + ro * math.cos(a0 + (a1 - a0) * i / n), cy + ro * math.sin(a0 + (a1 - a0) * i / n)) for i in range(n + 1)]
    inner = [P(cx + ri * math.cos(a1 - (a1 - a0) * i / n), cy + ri * math.sin(a1 - (a1 - a0) * i / n)) for i in range(n + 1)]
    band = (outer + inner)[::-1]                    # the same winding as the round caps (they union, never cancel)
    caps = [circle(cx + radius * math.cos(a), cy + radius * math.sin(a), STROKE / 2) for a in (a0, a1)]
    return [band] + caps


def build(path):
    s = STROKE / 2
    contours = [rrect(2 - s, 5 - s, 10 + s, 19 + s, 2 + s, True),      # the speaker box (outer edge) ...
                rrect(2 + s, 5 + s, 10 - s, 19 - s, 2 - s, False),     # ... and its hole
                circle(6, 14, 1.9)]                                    # the driver, a filled dot
    contours += wave(14, 8.5, 15.5, 5)
    contours += wave(17.5, 6, 18, 9)
    pen = TTGlyphPen(None)
    for c in contours:
        pen.moveTo(c[0])
        for p in c[1:]:
            pen.lineTo(p)
        pen.closePath()
    fb = FontBuilder(24 * UNIT, isTTF=True)
    fb.setupGlyphOrder([".notdef", "beam"])
    fb.setupCharacterMap({0xF8D0: "beam"})
    empty = TTGlyphPen(None).glyph()
    fb.setupGlyf({".notdef": empty, "beam": pen.glyph()})
    fb.setupHorizontalMetrics({".notdef": (24 * UNIT, 0), "beam": (24 * UNIT, 0)})
    fb.setupHorizontalHeader(ascent=24 * UNIT, descent=0)
    fb.setupNameTable({"familyName": "diskOS Beam", "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=24 * UNIT, sTypoDescender=0, usWinAscent=24 * UNIT, usWinDescent=0)
    fb.setupPost()
    fb.save(path)


if __name__ == "__main__":
    build(sys.argv[1])
