#!/usr/bin/env python3
"""Checks that every screen text fits the 128x64 OLED, in both languages.

Screen texts are the TRS("English", "Español") pairs in the firmware
(TR() is used for app and alert texts). For each one it checks:
  - plain ASCII only: the screen fonts have no accents or ñ,
  - headers (ALL CAPS, the small 6 px font): at most 12 characters (3.13), with
    "%lu:%02lu" counted as "18:42", so the tank litres, arrow and gauge stay clear,
  - messages: at most two parts ("a|b", one per line, 3.12), each at most 128 px
    wide in every screen font in Settings, measured as the firmware does
    (Adafruit GFX getTextBounds, from the font headers). From 3.9 each font
    has one fixed size for every message, so a wider page would scroll.
  - messages in the Large font (3.14, the default: the built-in font at twice
    its size): the whole message in at most three lines of 10 characters,
    every part starting on a new line and wrapped at word boundaries, as
    ScreenWrap.h does it; otherwise the firmware would drop that message to
    the small size. Checked with the longest values ("120:00", "125 min",
    "125L"), and for TRS_TIMED() texts together with the time left that the
    firmware adds under them,
  - every glyph of every two-line screen font, at the two baselines set in
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
    table = disp[disp.index('SCREEN_FONTS[SCREEN_FONT_LARGE]'):]
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
found = re.findall(r'TRS(_TIMED)?\("((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"\)', src)
pairs = [(en, es) for timed, en, es in found]

# ── Large font: as ScreenWrap.h (screenWrapAppend, screenMessageSize) ──
wrap_h = open(os.path.join(here, 'ScreenWrap.h'), encoding='utf-8').read()
LARGE_COLS  = int(re.search(r'#define SCREEN_LARGE_COLS\s+(\d+)', wrap_h).group(1))
LARGE_LINES = int(re.search(r'#define SCREEN_LARGE_LINES\s+(\d+)', wrap_h).group(1))
def wrap(text):
    """Lines of one part, or None if a word is longer than a line."""
    lines = []
    for word in text.split():
        if len(word) > LARGE_COLS:
            return None
        if lines and len(lines[-1]) + 1 + len(word) <= LARGE_COLS:
            lines[-1] += ' ' + word
        else:
            lines.append(word)
    return lines
def large_problem(message):
    """None if the message is shown in the Large size, otherwise why not."""
    parts = [wrap(p) for p in message.split('|')]
    if any(p is None for p in parts):
        return f'a word longer than {LARGE_COLS} characters'
    need = len(parts[0]) + max([len(p) for p in parts[1:]] or [0])
    if need > LARGE_LINES:
        return f'{need} lines: ' + ' / '.join(l for p in parts for l in p)
    return None
def longest(text):   # the longest values the firmware can put in
    return text.replace('%lu:%02lu', '120:00').replace('%lu', '125').replace('%.0f', '125').replace('%s', 'gw-a1b2c3')
def is_header(text):
    return all(p.upper() == p and any(c.isalpha() for c in p) for p in longest(text).split('|'))

large_bad, large_checked = [], 0
tails = [(en, es) for timed, en, es in found if en.startswith('%s|')]        # "%s|%lu min left"
if len(tails) != 1:
    large_bad.append(f'expected one "%s|..." time-left text, found {len(tails)}')
for timed, en, es in found:
    for lang, text in (('en', en), ('es', es)):
        if text.startswith('%s|') or is_header(text) or any(ord(c) > 126 for c in text):
            continue
        shown = longest(text)
        if timed and tails:
            shown += longest(tails[0][0 if lang == 'en' else 1])[len('gw-a1b2c3'):]
        large_checked += 1
        why = large_problem(shown)
        if why:
            large_bad.append(f'{lang} message "{shown}": not in the Large size ({why})')
# The litres of a finished cycle are joined at run time: "Reused 26L|Saved 31L".
done = [(en, es) for timed, en, es in found if '%.0f' in en]
if len(done) != 2:
    large_bad.append(f'expected the two litres texts of a finished cycle, found {len(done)}')
else:
    for lang in (0, 1):
        large_checked += 1
        shown = longest(done[0][lang] + '|' + done[1][lang])
        why = large_problem(shown)
        if why:
            large_bad.append(f'{"en" if lang == 0 else "es"} message "{shown}": not in the Large size ({why})')

bad, checked = [], 0
for en, es in pairs:
    for lang, text in (('en', en), ('es', es)):
        shown = text.replace('%lu:%02lu', '18:42').replace('%lu', '125').replace('%.0f', '26').replace('%s', 'gw-a1b2c3')
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
                if len(page) > 12:
                    bad.append(f'{lang} header "{page}": {len(page)} characters (max 12, beside the tank litres)')
                continue
            messages.append(page)
            for name, font in SCREEN_FONTS:
                if width(page, font) > 128:
                    bad.append(f'{lang} message page "{page}": {width(page, font)} px in {name} (max 128)')

bad += vertical_problems()
bad += large_bad
for b in bad:
    print('  FAIL ' + b)
for name, font in SCREEN_FONTS:
    widest = max(width(m, font) for m in messages)
    print(f'  screen font {name}: all {len(messages)} message pages in one size, widest {widest} px of 128')
print(f'  screen font Large: all {large_checked} messages in {LARGE_LINES} lines of {LARGE_COLS} characters or fewer')
print(f'{checked} screen texts and pages checked, {len(bad)} do not fit')
sys.exit(1 if bad else 0)
