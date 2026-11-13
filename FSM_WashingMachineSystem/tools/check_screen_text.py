#!/usr/bin/env python3
"""Checks that every screen text fits the 128x64 OLED, in both languages.

Screen texts are the TRS("English", "Español") pairs in the firmware
(TR() is used for app and alert texts). For each one it checks:
  - plain ASCII only: the screen fonts have no accents or ñ,
  - headers (ALL CAPS, the small 6 px font): at most 16 characters, with
    "%lu:%02lu" counted as "18:42", so the tank and wash icons stay clear,
  - messages: at most two parts ("a|b", one per line, 3.12), each at most 128 px
    wide in every screen font in Settings, measured as the firmware does
    (Adafruit GFX getTextBounds, from the font headers). From 3.9 each font
    has one fixed size for every message, so a wider page would scroll.
  - every glyph of every screen font, at the two baselines set in
    GreyWaterDisplay.cpp (top and bottom line), stays on rows 17-63: in the
    blue area of the 64-row screen with one blank row under the yellow rows
    (0-15), and the two lines at least 2 rows apart.

Usage: python3 tools/check_screen_text.py
Exit code 1 if anything does not fit.
"""
import glob, os, re, sys

here = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

# Glyph table: {bitmapOffset, width, height, xAdvance, xOffset, yOffset}, from 0x20.
def glyphs(name):
    src = open(os.path.join(here, name), encoding='utf-8').read()
    return [tuple(map(int, g)) for g in re.findall(r'\{\s*\d+,\s*(\d+),\s*\d+,\s*(\d+),\s*(-?\d+),\s*-?\d+\s*\}', src)]
# Rows of every glyph at the baseline the firmware uses (SCREEN_FONTS in GreyWaterDisplay.cpp).
def vertical_problems():
    disp = open(os.path.join(here, 'GreyWaterDisplay.cpp'), encoding='utf-8').read()
    table = disp[disp.index('SCREEN_FONTS[SCREEN_FONT_COUNT]'):]
    table = table[:table.index('};')]
    out = []
    for var, base1, base2 in re.findall(r'\{\s*&(\w+),\s*(\d+),\s*(\d+)\s*\}', table):
        src = open(os.path.join(here, f'Font{var}.h'), encoding='utf-8').read()
        rows = [(int(h), int(yo)) for h, yo in
            re.findall(r'\{\s*\d+,\s*\d+,\s*(\d+),\s*\d+,\s*-?\d+,\s*(-?\d+)\s*\}', src) if int(h)]
        span = lambda base: (min(int(base) + yo for h, yo in rows), max(int(base) + yo + h - 1 for h, yo in rows))
        (top1, bottom1), (top2, bottom2) = span(base1), span(base2)
        print(f'  font {var}: baselines {base1} and {base2}, glyphs on rows {top1}-{bottom1} and {top2}-{bottom2}')
        if top1 < 17 or bottom2 > 63:
            out.append(f'font {var}: glyphs on rows {top1}-{bottom2}, outside the message area (rows 17-63)')
        if top2 - bottom1 < 3:
            out.append(f'font {var}: the top line ends on row {bottom1} and the bottom line starts on row {top2} (need 2 blank rows)')
    if not table.count('&'):
        out.append('no screen fonts found in SCREEN_FONTS (GreyWaterDisplay.cpp)')
    return out

# App label and the one size used for every message (as SCREEN_FONTS in GreyWaterDisplay.cpp)
SCREEN_FONTS = [('Hyperlegible', glyphs('FontHyperlegible12.h')),
                ('Plain sans', glyphs('FontPlainSans13.h')),
                ('Terminal bold', glyphs('FontTerminal16.h')),
                ('Schoolbook bold', glyphs('FontSchoolbook10.h'))]
messages = []
def width(text, font):
    """Width as getTextBounds reports it: from the leftmost to the rightmost glyph box."""
    x, lo, hi = 0, 10**6, -10**6
    for c in text:
        w, adv, xo = font[ord(c) - 0x20]
        lo, hi = min(lo, x + xo), max(hi, x + xo + w - 1)
        x += adv
    return hi - lo + 1 if text else 0

src = ''
for f in glob.glob(os.path.join(here, '*.cpp')) + glob.glob(os.path.join(here, '*.ino')):
    src += open(f, encoding='utf-8').read()
pairs = re.findall(r'TRS\("((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"\)', src)

bad, checked = [], 0
for en, es in pairs:
    for lang, text in (('en', en), ('es', es)):
        shown = text.replace('Version %s', 'Version 3.12.0').replace('%lu:%02lu', '18:42').replace('%lu', '125').replace('%.0f', '26').replace('%s', 'gw-a1b2c3')
        if any(ord(c) > 126 for c in shown):
            checked += 1
            bad.append(f'{lang} "{text}": not plain ASCII (the screen cannot show it)')
            continue
        if shown.count('|') > 1 and not (shown.startswith('%s|') and shown.count('|') == 1):
            checked += 1
            bad.append(f'{lang} "{text}": {shown.count("|") + 1} parts (the screen has two lines, 3.12)')
            continue
        for page in shown.split('|'):
            if page in ('', 'gw-a1b2c3'):  # "%s|..." joins pages built elsewhere; the setup key alone
                continue
            checked += 1
            if page.upper() == page and any(c.isalpha() for c in page):
                if len(page) > 16:
                    bad.append(f'{lang} header "{page}": {len(page)} characters (max 16)')
                continue
            messages.append(page)
            for name, font in SCREEN_FONTS:
                if width(page, font) > 128:
                    bad.append(f'{lang} message page "{page}": {width(page, font)} px in {name} (max 128)')

bad += vertical_problems()
for b in bad:
    print('  FAIL ' + b)
for name, font in SCREEN_FONTS:
    widest = max(width(m, font) for m in messages)
    print(f'  screen font {name}: all {len(messages)} message pages in one size, widest {widest} px of 128')
print(f'{checked} screen texts and pages checked, {len(bad)} do not fit')
sys.exit(1 if bad else 0)
