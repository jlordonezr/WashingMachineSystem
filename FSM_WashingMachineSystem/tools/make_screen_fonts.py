#!/usr/bin/env python3
"""Builds the screen fonts (Settings -> Screen font) as Adafruit GFX headers.

Each style has a main size and a smaller size of the same family. A message
page too wide for the main size is shown in the smaller one, which every
screen text fits (tools/check_screen_text.py checks it). ASCII 0x20-0x7E,
1 bit per pixel. The source files are not stored in this repository; download
them into one folder first (all are free to embed):

  AtkinsonHyperlegible-Bold.ttf  github.com/google/fonts  ofl/atkinsonhyperlegible   SIL OFL 1.1  -> FontHyperlegible13.h, FontHyperlegible12.h
  PT_Sans-Web-Bold.ttf           github.com/google/fonts  ofl/ptsans                 SIL OFL 1.1  -> FontPlainSans14.h, FontPlainSans13.h
  ter-u20b.bdf, ter-u16b.bdf     terminus-font.sourceforge.net (terminus-font-4.49.1) SIL OFL 1.1  -> FontTerminal20.h, FontTerminal16.h
  ncenB14.bdf, ncenB10.bdf       github.com/olikraus/u8g2 tools/font/bdf             Adobe X11    -> FontSchoolbook14.h, FontSchoolbook10.h

TrueType fonts are drawn with FreeType's monochrome hinting, as Adafruit's
fontconvert does (needs: pip install freetype-py). Run from the sketch folder:

    python3 tools/make_screen_fonts.py <folder with the files above>
"""
import os, sys

ADOBE = ('Copyright 1984-1989, 1994 Adobe Systems Incorporated.\n'
         '//   Copyright 1988, 1994 Digital Equipment Corporation.\n'
         '//\n'
         '//   Adobe is a trademark of Adobe Systems Incorporated which may be\n'
         '//   registered in certain jurisdictions.\n'
         '//   Permission to use these trademarks is hereby granted only in\n'
         '//   association with the images described in this file.\n'
         '//\n'
         '//   Permission to use, copy, modify, distribute and sell this software\n'
         '//   and its documentation for any purpose and without fee is hereby\n'
         '//   granted, provided that the above copyright notices appear in all\n'
         '//   copies and that both those copyright notices and this permission\n'
         '//   notice appear in supporting documentation, and that the names of\n'
         '//   Adobe Systems and Digital Equipment Corporation not be used in\n'
         '//   advertising or publicity pertaining to distribution of the software\n'
         '//   without specific, written prior permission.  Adobe Systems and\n'
         '//   Digital Equipment Corporation make no representations about the\n'
         '//   suitability of this software for any purpose.  It is provided "as\n'
         '//   is" without express or implied warranty.')
ATKINSON = ('Copyright 2020 Braille Institute of America, Inc.\n'
            '//   Licensed under the SIL Open Font License 1.1 (https://openfontlicense.org), licenses/OFL-Hyperlegible.txt.')
PTSANS = ('Copyright (c) 2010, ParaType Ltd. (http://www.paratype.com/public). Licensed under the SIL Open Font\n'
          '//   License 1.1 (https://openfontlicense.org), Reserved Font Names "PT Sans" and "ParaType"; licenses/OFL-PlainSans.txt.')
TERMINUS = ('Copyright (C) 2020 Dimitar Toshkov Zhekov. Licensed under the SIL Open Font License 1.1\n'
            '//   (https://openfontlicense.org), Reserved Font Name "Terminus Font"; licenses/OFL-Terminal.txt.')

# Converted fonts are "Modified Versions" under the SIL OFL, so they do not use
# a Reserved Font Name (PT Sans, Terminus Font): the header, variable and app
# label describe the style instead, and the notice names the original.
FONTS = [  # file, capital height (TrueType only), header, variable, description, licence note, space narrower by
    ('AtkinsonHyperlegible-Bold.ttf', 13, 'FontHyperlegible13.h', 'Hyperlegible13', 'hyperlegible bold sans, derived from Atkinson Hyperlegible Bold', ATKINSON, 0),
    # word space 1 px narrower, so "Wi-Fi connected" fits 128 px
    ('AtkinsonHyperlegible-Bold.ttf', 12, 'FontHyperlegible12.h', 'Hyperlegible12', 'hyperlegible bold sans, derived from Atkinson Hyperlegible Bold', ATKINSON, 1),
    ('PT_Sans-Web-Bold.ttf', 14, 'FontPlainSans14.h', 'PlainSans14', 'plain bold sans, derived from PT Sans Bold', PTSANS, 0),
    ('PT_Sans-Web-Bold.ttf', 13, 'FontPlainSans13.h', 'PlainSans13', 'plain bold sans, derived from PT Sans Bold', PTSANS, 0),
    ('ter-u20b.bdf', None, 'FontTerminal20.h', 'Terminal20', 'terminal bold 10x20 pixel font, derived from Terminus Font (ter-u20b)', TERMINUS, 0),
    ('ter-u16b.bdf', None, 'FontTerminal16.h', 'Terminal16', 'terminal bold 8x16 pixel font, derived from Terminus Font (ter-u16b)', TERMINUS, 0),
    ('ncenB14.bdf', None, 'FontSchoolbook14.h', 'Schoolbook14', 'Adobe New Century Schoolbook Bold pixel font (X11 "ncenB14.bdf")', ADOBE, 0),
    ('ncenB10.bdf', None, 'FontSchoolbook10.h', 'Schoolbook10', 'Adobe New Century Schoolbook Bold pixel font (X11 "ncenB10.bdf")', ADOBE, 0),
]

def from_ttf(path, cap):
    import freetype
    f = freetype.Face(path)
    flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO
    for px in range(8, 40):                       # smallest size whose "H" is cap px tall
        f.set_pixel_sizes(0, px)
        f.load_char('H', flags)
        if f.glyph.bitmap.rows >= cap: break
    glyphs = {}
    for code in range(32, 127):
        f.load_char(chr(code), flags)
        bm = f.glyph.bitmap
        rows = []
        for y in range(bm.rows):
            row = bm.buffer[y * bm.pitch:(y + 1) * bm.pitch]
            rows.append([1 if row[x // 8] & (0x80 >> (x % 8)) else 0 for x in range(bm.width)])
        glyphs[code] = (f.glyph.advance.x >> 6, f.glyph.bitmap_left, f.glyph.bitmap_top, rows)
    return glyphs

def from_bdf(path):
    glyphs, cur, rows, inbm = {}, None, [], False
    for line in open(path, encoding='latin-1'):
        t = line.split()
        if not t: continue
        if t[0] == 'ENCODING': cur = {'enc': int(t[1])}
        elif t[0] == 'DWIDTH' and cur is not None: cur['dw'] = int(t[1])
        elif t[0] == 'BBX' and cur is not None: cur['bbx'] = tuple(map(int, t[1:5]))
        elif t[0] == 'BITMAP': inbm, rows = True, []
        elif t[0] == 'ENDCHAR':
            inbm = False
            if cur and 32 <= cur['enc'] <= 126:
                w, h, xo, yo = cur['bbx']
                bits = [[(int(r, 16) >> (len(r) * 4 - 1 - x)) & 1 for x in range(w)] for r in rows]
                glyphs[cur['enc']] = (cur['dw'], xo, yo + h, bits)
            cur = None
        elif inbm: rows.append(t[0])
    return glyphs

def header(glyphs, var, name, note, source, cap):
    bits, table = [], []
    for code in range(32, 127):
        adv, left, top, rows = glyphs[code]
        w = len(rows[0]) if rows else 0
        if w == 0: rows = []
        table.append((len(bits) // 8, w, len(rows), adv, left, -top if rows else 0, code))
        for r in rows: bits.extend(r)
        while len(bits) % 8: bits.append(0)
    data = [int(''.join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8)]
    guard = var.upper() + '_H'
    out = [f'#ifndef {guard}', f'#define {guard}', '',
           f'// {var}: {name}; capitals {cap} px tall, ASCII 0x20-0x7E, converted to the',
           f'// Adafruit GFX font format by tools/make_screen_fonts.py from {source}.',
           f'//   {note}', '', '#include <Adafruit_GFX.h>', '',
           f'static const uint8_t {var}Bitmaps[] PROGMEM = {{']
    for i in range(0, len(data), 16):
        out.append('  ' + ', '.join(f'0x{b:02X}' for b in data[i:i + 16]) + ',')
    out += ['};', '', f'static const GFXglyph {var}Glyphs[] PROGMEM = {{']
    for o, w, h, a, xo, yo, code in table:
        ch = chr(code).replace('\\', '\\\\')
        out.append(f"  {{ {o:5d}, {w:2d}, {h:2d}, {a:2d}, {xo:3d}, {yo:3d} }},   // 0x{code:02X} '{ch}'")
    out += ['};', '', f'static const GFXfont {var} PROGMEM = {{',
            f'  (uint8_t*){var}Bitmaps, (GFXglyph*){var}Glyphs, 0x20, 0x7E, 20 }};', '', '#endif', '']
    return '\n'.join(out)

if __name__ == '__main__':
    src = sys.argv[1] if len(sys.argv) > 1 else sys.exit(__doc__)
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    for file, cap, hdr, var, name, note, narrower in FONTS:
        path = os.path.join(src, file)
        glyphs = from_bdf(path) if file.endswith('.bdf') else from_ttf(path, cap)
        if narrower:
            a, l, t, r = glyphs[32]
            glyphs[32] = (a - narrower, l, t, r)
        h = glyphs[ord('H')]
        cap = h[2] if h[3] else cap
        open(os.path.join(here, hdr), 'w', encoding='utf-8').write(header(glyphs, var, name, note, file, cap))
        print(f'{hdr}: {name}')
