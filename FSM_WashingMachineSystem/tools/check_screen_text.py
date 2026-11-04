#!/usr/bin/env python3
"""Checks that every screen text fits the 128x32 OLED, in both languages.

Screen texts are the TRS("English", "Español") pairs in the firmware
(TR() is used for app and alert texts). For each one it checks:
  - plain ASCII only: the screen fonts have no accents or ñ,
  - headers (ALL CAPS, the small 6 px font): at most 16 characters, with
    "%lu:%02lu" counted as "18:42", so the tank and wash icons stay clear,
  - messages: every page ("a|b|c", shown one after the other) at most 128 px
    wide in the smaller size of every screen font in Settings (3.6), measured
    as the firmware does (Adafruit GFX getTextBounds, from the font headers).
    The screen uses the main size when a page fits, else the smaller size of
    the same font; wider pages would scroll.
  - for each screen font, how many pages show in its main size (information,
    not an error).

Usage: python3 tools/check_screen_text.py
Exit code 1 if anything does not fit.
"""
import glob, os, re, sys

here = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

# Glyph table: {bitmapOffset, width, height, xAdvance, xOffset, yOffset}, from 0x20.
def glyphs(name):
    src = open(os.path.join(here, name), encoding='utf-8').read()
    return [tuple(map(int, g)) for g in re.findall(r'\{\s*\d+,\s*(\d+),\s*\d+,\s*(\d+),\s*(-?\d+),\s*-?\d+\s*\}', src)]
# App label, main size, smaller size (as SCREEN_FONTS in GreyWaterDisplay.cpp)
SCREEN_FONTS = [('Hyperlegible', glyphs('FontHyperlegible13.h'), glyphs('FontHyperlegible12.h')),
                ('Plain sans', glyphs('FontPlainSans14.h'), glyphs('FontPlainSans13.h')),
                ('Terminal bold', glyphs('FontTerminal20.h'), glyphs('FontTerminal16.h')),
                ('Schoolbook bold', glyphs('FontSchoolbook14.h'), glyphs('FontSchoolbook10.h'))]
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
        shown = text.replace('%lu:%02lu', '18:42').replace('%lu', '125').replace('%.0f', '26').replace('%s', 'gw-a1b2c3')
        if any(ord(c) > 126 for c in shown):
            checked += 1
            bad.append(f'{lang} "{text}": not plain ASCII (the screen cannot show it)')
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
            for name, main, small in SCREEN_FONTS:
                if width(page, small) > 128:
                    bad.append(f'{lang} message page "{page}": {width(page, small)} px in {name}, smaller size (max 128)')

for b in bad:
    print('  FAIL ' + b)
for name, main, small in SCREEN_FONTS:
    own = sum(1 for m in messages if width(m, main) <= 128)
    print(f'  screen font {name}: {own} of {len(messages)} message pages in the main size, the rest in the smaller size')
print(f'{checked} screen texts and pages checked, {len(bad)} do not fit')
sys.exit(1 if bad else 0)
