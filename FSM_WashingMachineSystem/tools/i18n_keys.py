#!/usr/bin/env python3
"""Lists every English text the dashboard can show, for web/es.json.

Sources: static page text and aria-label/title/placeholder attributes,
t('...') calls in the script, the lists the script passes through t()
(STEP_NAMES, LEVEL, WASH_END, PLAIN, hints), and messages the firmware sends
that the page translates (errors and history sources in GreyWaterNet.cpp /
GreyWaterSettings.cpp).

python3 tools/i18n_keys.py          -> prints keys missing from web/es.json (exit 1 if any)
python3 tools/i18n_keys.py --all    -> prints every key
"""
import html, json, os, re, sys
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
page = open(os.path.join(root, 'web', 'dashboard.html'), encoding='utf-8').read()
i = page.index('<script>'); markup, script = page[:i], page[i:]
keys = []
def add(k):
    k = html.unescape(k).strip()
    if k and re.search('[A-Za-z]', k) and '+(' not in k and k not in keys: keys.append(k)

m = re.search(r'<title>(.*?)</title>', markup); add(m.group(1))
body = re.sub(r'<style>.*?</style>', '', markup, flags=re.S)
body = body[body.index('<body'):]
for txt in re.findall(r'>([^<>]+)<', body):
    if txt.strip() not in ('Español', 'English'): add(txt)
for a in re.findall(r'(?:aria-label|title|placeholder)="([^"]+)"', body): add(a)
# the rest of the page after the script (dialogs etc.) is part of `markup`/`script` split; scan it too
tail = page[page.index('</script>'):]
for txt in re.findall(r'>([^<>]+)<', tail): add(txt)

js = script.replace("\\u2019", "’").replace("\\u2026", "…")
for k in re.findall(r"\bt\('((?:[^'\\]|\\.)*)'", js): add(k)
def lit(name):
    m = re.search(name + r"\s*=\s*([\[{].*?[\]}]);", js, re.S)
    return re.findall(r"'((?:[^'\\]|\\.)*)'", m.group(1)) if m else []
for k in lit('STEP_NAMES'): add(k)
for k in re.findall(r"t:'([^']+)'", js): add(k)
for k in lit('WASH_END'):
    if ' ' in k: add(k)
plain = re.search(r"var PLAIN=\{(.*?)\};", js, re.S).group(1)
for k in re.findall(r":'((?:[^'\\]|\\.)*)'", plain): add(k)
for k in re.findall(r"hint='((?:[^'\\]|\\.)*)'", js): add(k)
fw = open(os.path.join(root, 'GreyWaterNet.cpp'), encoding='utf-8').read()
for k in re.findall(r'sendError\(\d+,\s*"([^"]+)"\)', fw): add(k)
st = open(os.path.join(root, 'GreyWaterSettings.cpp'), encoding='utf-8').read()
for k in re.findall(r'return "([^"]+)";', st[st.index('historySourceName'):]): add(k)

if '--all' in sys.argv:
    print('\n'.join(keys)); sys.exit(0)
es = json.load(open(os.path.join(root, 'web', 'es.json'), encoding='utf-8')) if os.path.exists(os.path.join(root, 'web', 'es.json')) else {}
missing = [k for k in keys if k not in es]
unused = [k for k in es if k not in keys]
for k in missing: print('missing: ' + k)
for k in unused: print('unused: ' + k)
print(f'{len(keys)} texts, {len(missing)} missing, {len(unused)} unused')
sys.exit(1 if missing else 0)
