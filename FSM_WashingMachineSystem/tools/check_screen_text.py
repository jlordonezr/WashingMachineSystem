#!/usr/bin/env python3
"""Checks that every screen text fits the 128x32 OLED, in both languages.

Screen texts are the TRS("English", "Español") pairs in the firmware
(TR() is used for app and alert texts). For each one it checks:
  - plain ASCII only: the screen fonts have no accents or ñ,
  - headers (ALL CAPS, the small 6 px font): at most 16 characters, with
    "%lu:%02lu" counted as "18:42", so the tank and wash icons stay clear,
  - messages (FreeSans 9 pt): at most 128 px wide, measured with the real
    glyph widths from the Adafruit GFX font file. Wider texts would scroll.

Usage: python3 tools/check_screen_text.py [path/to/Fonts/FreeSans9pt7b.h]
Exit code 1 if anything does not fit.
"""
import glob, os, re, sys

here = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
font = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
    '~/Arduino/libraries/Adafruit_GFX_Library/Fonts/FreeSans9pt7b.h')
if not os.path.exists(font):
    alt = glob.glob(os.path.expanduser('~/Arduino/libraries/Adafruit*GFX*/Fonts/FreeSans9pt7b.h'))
    font = alt[0] if alt else font
if not os.path.exists(font):
    print('skipped: FreeSans9pt7b.h not found (install Adafruit GFX Library or pass its path)')
    sys.exit(0)

# Glyph table: {bitmapOffset, width, height, xAdvance, xOffset, yOffset}, from 0x20.
glyphs = re.findall(r'\{\s*\d+,\s*\d+,\s*\d+,\s*(\d+),\s*-?\d+,\s*-?\d+\s*\}', open(font).read())
adv = [int(a) for a in glyphs]
def width(text):
    return sum(adv[ord(c) - 0x20] for c in text)

src = ''
for f in glob.glob(os.path.join(here, '*.cpp')) + glob.glob(os.path.join(here, '*.ino')):
    src += open(f, encoding='utf-8').read()
pairs = re.findall(r'TRS\("((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"\)', src)

bad, checked = [], 0
for en, es in pairs:
    for lang, text in (('en', en), ('es', es)):
        shown = text.replace('%lu:%02lu', '18:42').replace('%.0f', '26').replace('%s', 'gw-a1b2c3')
        checked += 1
        if any(ord(c) > 126 for c in shown):
            bad.append(f'{lang} "{text}": not plain ASCII (the screen cannot show it)')
        elif shown.upper() == shown and any(c.isalpha() for c in shown):
            if len(shown) > 16:
                bad.append(f'{lang} header "{shown}": {len(shown)} characters (max 16)')
        elif width(shown) > 128:
            bad.append(f'{lang} message "{shown}": {width(shown)} px (max 128)')

for b in bad:
    print('  FAIL ' + b)
print(f'{checked} screen texts checked, {len(bad)} do not fit')
sys.exit(1 if bad else 0)
