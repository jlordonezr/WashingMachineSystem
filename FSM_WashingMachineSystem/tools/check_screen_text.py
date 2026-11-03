#!/usr/bin/env python3
"""Checks that every screen text fits the 128x32 OLED, in both languages.

Screen texts are the TRS("English", "Español") pairs in the firmware
(TR() is used for app and alert texts). For each one it checks:
  - plain ASCII only: the screen fonts have no accents or ñ,
  - headers (ALL CAPS, the small 6 px font): at most 16 characters, with
    "%lu:%02lu" counted as "18:42", so the tank and wash icons stay clear,
  - messages (Helvetica Bold pixel font): every page ("a|b|c", shown one
    after the other) at most 128 px wide in the 12 px size, measured with the
    real glyph widths from FontHelvBold12.h. The screen uses 14 px when a page
    fits, else 12 px; wider pages would scroll.

Usage: python3 tools/check_screen_text.py
Exit code 1 if anything does not fit.
"""
import glob, os, re, sys

here = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

# Glyph table: {bitmapOffset, width, height, xAdvance, xOffset, yOffset}, from 0x20.
def advances(name):
    src = open(os.path.join(here, name), encoding='utf-8').read()
    return [int(a) for a in re.findall(r'\{\s*\d+,\s*\d+,\s*\d+,\s*(\d+),\s*-?\d+,\s*-?\d+\s*\}', src)]
adv12, adv14 = advances('FontHelvBold12.h'), advances('FontHelvBold14.h')
def width(text, adv=adv12):
    return sum(adv[ord(c) - 0x20] for c in text)

src = ''
for f in glob.glob(os.path.join(here, '*.cpp')) + glob.glob(os.path.join(here, '*.ino')):
    src += open(f, encoding='utf-8').read()
pairs = re.findall(r'TRS\("((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"\)', src)

bad, checked, large = [], 0, 0
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
            elif width(page) > 128:
                bad.append(f'{lang} message page "{page}": {width(page)} px (max 128)')
            elif width(page, adv14) <= 128:
                large += 1

for b in bad:
    print('  FAIL ' + b)
print(f'{checked} screen texts and pages checked, {len(bad)} do not fit ({large} messages shown in 14 px)')
sys.exit(1 if bad else 0)
