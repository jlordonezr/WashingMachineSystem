#!/usr/bin/env python3
"""Builds the optional screen fonts (Settings -> Screen font) as Adafruit GFX headers.

Each font is drawn with capitals 13 px tall, ASCII 0x20-0x7E, 1 bit per pixel.
The source files are not stored in this repository; download them into one
folder first (all are free to embed):

  FiraSansCondensed-Bold.ttf     github.com/google/fonts  ofl/firasanscondensed    SIL OFL 1.1  -> FontNarrowBold13.h
  PT_Sans-Narrow-Web-Bold.ttf    github.com/google/fonts  ofl/ptsansnarrow         SIL OFL 1.1  -> FontNarrowLight13.h
  BarlowSemiCondensed-Bold.ttf   github.com/google/fonts  ofl/barlowsemicondensed  SIL OFL 1.1  -> FontSemiNarrow13.h
  Lato-Bold.ttf                  github.com/google/fonts  ofl/lato                 SIL OFL 1.1  -> FontRound13.h
  timB14.bdf                     github.com/olikraus/u8g2 tools/font/bdf           Adobe X11    -> FontSerifBold13.h

TrueType fonts are drawn with FreeType's monochrome hinting, as Adafruit's
fontconvert does (needs: pip install freetype-py). Run from the sketch folder:

    python3 tools/make_screen_fonts.py <folder with the files above>
"""
import os, sys

# Converted fonts are "Modified Versions" under the SIL OFL, so they do not use
# a Reserved Font Name (Fira, PT Sans, Lato): the header, variable and app
# label describe the style instead, and the notice names the original.
FONTS = [  # file, header, variable, description, licence note
    ('FiraSansCondensed-Bold.ttf', 'FontNarrowBold13.h', 'NarrowBold13', 'narrow bold sans, derived from Fira Sans Condensed Bold',
     'Digitized data copyright 2012-2016, The Mozilla Foundation and Telefonica S.A.\n'
     '//   Licensed under the SIL Open Font License 1.1 (https://openfontlicense.org), Reserved Font Name "Fira".'),
    ('PT_Sans-Narrow-Web-Bold.ttf', 'FontNarrowLight13.h', 'NarrowLight13', 'narrow light sans, derived from PT Sans Narrow Bold',
     'Copyright 2009-2010 ParaType Ltd. Licensed under the SIL Open Font License 1.1\n'
     '//   (https://openfontlicense.org), Reserved Font Names "PT Sans" and "ParaType".'),
    ('BarlowSemiCondensed-Bold.ttf', 'FontSemiNarrow13.h', 'SemiNarrow13', 'semi-narrow bold sans, derived from Barlow Semi Condensed Bold',
     'Copyright 2017 The Barlow Project Authors (https://github.com/jpt/barlow).\n'
     '//   Licensed under the SIL Open Font License 1.1 (https://openfontlicense.org).'),
    ('Lato-Bold.ttf', 'FontRound13.h', 'Round13', 'round bold sans, derived from Lato Bold',
     'Copyright (c) 2011-2015 by tyPoland Lukasz Dziedzic (http://www.typoland.com/).\n'
     '//   Licensed under the SIL Open Font License 1.1 (https://openfontlicense.org), Reserved Font Name "Lato".'),
    ('timB14.bdf', 'FontSerifBold13.h', 'SerifBold13', 'Adobe Times Bold pixel font (X11 "timB14.bdf")',
     'Copyright 1984-1989, 1994 Adobe Systems Incorporated; 1988, 1994 Digital Equipment Corporation.\n'
     '//   Same permission notice as FontHelvBold12.h (free use, copy and distribution with this notice).'),
]
CAP = 13

def from_ttf(path):
    import freetype
    f = freetype.Face(path)
    flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO
    for px in range(8, 40):                       # smallest size whose "H" is CAP px tall
        f.set_pixel_sizes(0, px)
        f.load_char('H', flags)
        if f.glyph.bitmap.rows >= CAP: break
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

def header(glyphs, var, name, note, source):
    bits, table = [], []
    for code in range(32, 127):
        adv, left, top, rows = glyphs[code]
        w = len(rows[0]) if rows else 0
        table.append((len(bits) // 8, w, len(rows), adv, left, -top, code))
        for r in rows: bits.extend(r)
        while len(bits) % 8: bits.append(0)
    data = [int(''.join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8)]
    guard = var.upper() + '_H'
    out = [f'#ifndef {guard}', f'#define {guard}', '',
           f'// {var}: {name}; capitals {CAP} px tall, ASCII 0x20-0x7E, converted to the',
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
            f'  (uint8_t*){var}Bitmaps, (GFXglyph*){var}Glyphs, 0x20, 0x7E, 18 }};', '', '#endif', '']
    return '\n'.join(out)

if __name__ == '__main__':
    src = sys.argv[1] if len(sys.argv) > 1 else sys.exit(__doc__)
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    for file, hdr, var, name, note in FONTS:
        path = os.path.join(src, file)
        glyphs = from_bdf(path) if file.endswith('.bdf') else from_ttf(path)
        open(os.path.join(here, hdr), 'w', encoding='utf-8').write(header(glyphs, var, name, note, file))
        print(f'{hdr}: {name}')
