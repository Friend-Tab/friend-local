#!/usr/bin/env python3
"""phosphor_svg.py — SVG paths for the icons the page uses, out of the
Phosphor font the desktop window already draws with, so that the two
look the same and nothing new is vendored. Reads vendor/Phosphor.ttf
(TrueType outlines) and prints a JavaScript object of icon name to
path, in a 256-unit box, for app.js. The font has no glyph names, so an
icon is asked for by its codepoint, as frontend/sdl/ui_icons.h does:

    python3 frontend/web/tools/phosphor_svg.py send=e06c gear=e270 ...

A one-off: run it when an icon is added, and paste what it prints."""

import struct
import sys

STANDARD_MAC_NAMES_COUNT = 258  # post format 2's built-in names


def tables(font):
    count = struct.unpack('>H', font[4:6])[0]
    out = {}
    for i in range(count):
        tag, _, offset, length = struct.unpack('>4sIII', font[12 + 16 * i:28 + 16 * i])
        out[tag.decode()] = font[offset:offset + length]
    return out


def glyph_for(cmap, codepoint):
    """The glyph a codepoint draws, from a format 4 or 12 cmap."""
    count = struct.unpack('>H', cmap[2:4])[0]
    for i in range(count):
        platform, encoding, offset = struct.unpack('>HHI', cmap[4 + 8 * i:12 + 8 * i])
        sub = cmap[offset:]
        fmt = struct.unpack('>H', sub[0:2])[0]
        if fmt == 12:
            groups = struct.unpack('>I', sub[12:16])[0]
            for k in range(groups):
                lo, hi, first = struct.unpack('>III', sub[16 + 12 * k:28 + 12 * k])
                if lo <= codepoint <= hi:
                    return first + codepoint - lo
        elif fmt == 4:
            segs = struct.unpack('>H', sub[6:8])[0] // 2
            ends = struct.unpack('>%dH' % segs, sub[14:14 + 2 * segs])
            base = 16 + 2 * segs
            starts = struct.unpack('>%dH' % segs, sub[base:base + 2 * segs])
            deltas = struct.unpack('>%dh' % segs, sub[base + 2 * segs:base + 4 * segs])
            ro_at = base + 4 * segs
            ranges = struct.unpack('>%dH' % segs, sub[ro_at:ro_at + 2 * segs])
            for k in range(segs):
                if starts[k] <= codepoint <= ends[k]:
                    if ranges[k] == 0:
                        return (codepoint + deltas[k]) & 0xFFFF
                    at = ro_at + 2 * k + ranges[k] + 2 * (codepoint - starts[k])
                    g = struct.unpack('>H', sub[at:at + 2])[0]
                    return (g + deltas[k]) & 0xFFFF if g else 0
    return 0


def outline(t, glyph):
    head, loca, glyf = t['head'], t['loca'], t['glyf']
    long_offsets = struct.unpack('>h', head[50:52])[0] == 1
    if long_offsets:
        start, end = struct.unpack('>II', loca[4 * glyph:4 * glyph + 8])
    else:
        start, end = (2 * v for v in struct.unpack('>HH', loca[2 * glyph:2 * glyph + 4]))
    g = glyf[start:end]
    if not g:
        return ''
    contours = struct.unpack('>h', g[0:2])[0]
    if contours < 0:
        sys.exit('composite glyphs are not handled')
    ends = struct.unpack('>%dH' % contours, g[10:10 + 2 * contours])
    at = 10 + 2 * contours
    at += 2 + struct.unpack('>H', g[at:at + 2])[0]
    points = ends[-1] + 1
    flags = []
    while len(flags) < points:
        f = g[at]; at += 1
        flags.append(f)
        if f & 8:
            flags.extend([f] * g[at]); at += 1
    def coords(short, same):
        vals, v = [], 0
        nonlocal at
        for f in flags:
            if f & short:
                d = g[at]; at += 1
                v += d if f & same else -d
            elif not f & same:
                v += struct.unpack('>h', g[at:at + 2])[0]; at += 2
            vals.append(v)
        return vals
    xs = coords(2, 16)
    ys = coords(4, 32)
    units = struct.unpack('>H', head[18:20])[0]
    ascent = struct.unpack('>h', t['hhea'][4:6])[0]
    scale = 256.0 / units
    def pt(i):
        return (xs[i] * scale, (ascent - ys[i]) * scale)
    def fmt(p):
        return '%s %s' % tuple(('%.1f' % c).rstrip('0').rstrip('.') for c in p)
    d = []
    first = 0
    for last in ends:
        idx = list(range(first, last + 1))
        on = [flags[i] & 1 for i in idx]
        pts = [pt(i) for i in idx]
        # Start on an on-curve point, or the midpoint of two off ones.
        if on[0]:
            start_p = pts[0]; order = list(range(1, len(idx))) + [0]
        elif on[-1]:
            start_p = pts[-1]; order = list(range(0, len(idx)))
        else:
            start_p = ((pts[0][0] + pts[-1][0]) / 2, (pts[0][1] + pts[-1][1]) / 2)
            order = list(range(0, len(idx)))
        d.append('M' + fmt(start_p))
        control = None
        for k in order:
            p = pts[k]
            if on[k]:
                d.append(('Q%s %s' % (fmt(control), fmt(p))) if control else 'L' + fmt(p))
                control = None
            else:
                if control:
                    mid = ((control[0] + p[0]) / 2, (control[1] + p[1]) / 2)
                    d.append('Q%s %s' % (fmt(control), fmt(mid)))
                control = p
        if control:
            d.append('Q%s %s' % (fmt(control), fmt(start_p)))
        d.append('Z')
        first = last + 1
    return ''.join(d)


def main():
    font = open('vendor/Phosphor.ttf', 'rb').read()
    t = tables(font)
    print('const ICONS = {')
    for arg in sys.argv[1:]:
        name, code = arg.split('=')
        glyph = glyph_for(t['cmap'], int(code, 16))
        if not glyph:
            sys.exit('nothing at U+%s' % code)
        print("  '%s': '%s'," % (name, outline(t, glyph)))
    print('};')


main()
