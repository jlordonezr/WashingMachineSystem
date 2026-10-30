// Dashboard tests: the real page (web/dashboard.html) in headless Chromium,
// against a mock controller that answers /api/* like the firmware does.
// Usage (from the sketch folder): node tests/app/test_dashboard.js
// Needs Node.js and Playwright with Chromium (npm i playwright; npx playwright install chromium).
const http = require('http');
const fs = require('fs');
const path = require('path');
const { chromium } = require('playwright');

// The page as the firmware serves it: web/dashboard.html with the Spanish texts
// from web/es.json inserted (tools/build_dashboard.py does the same).
const ES = JSON.parse(fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'es.json'), 'utf8'));
const PAGE = fs.readFileSync(path.join(__dirname, '..', '..', 'web', 'dashboard.html'), 'utf8')
  .replace('/*ES-DICTIONARY*/{}', JSON.stringify(ES).replace(/<\//g, '<\\/'));

// ── mock controller ──
function baseStatus() {
  return {
    fw: '3.1.0', lang: 'en', state: 0, stateName: 'Ready', title: 'READY', message: 'Press START', tank: 'medium', wash: false,
    tapLpm: 0, drainLpm: 0, stepElapsedS: 0, drain2LeftS: 0, stuck: false, tankAgeH: 5, tankMaxAgeH: 48,
    allowRemoteStart: true, alerts: true, manualWash: false, manualWashLeftS: 0, manualWashMaxMin: 10, manualWashResult: '',
    cycle: { tap: 0, reused: 0, saved: 0 }, month: { key: 202610, tap: 10, reused: 20, saved: 30, cycles: 2 },
    total: { tap: 10, reused: 20, saved: 30, cycles: 2 }, prevMonth: { key: 0, tap: 0, reused: 0, saved: 0, cycles: 0 },
    last: { valid: true, agoS: 3600, durationS: 3000, tap: 5, reused: 10, saved: 15, completed: true },
    history: { source: 'Loaded from the controller', hasData: true }, fill: { litres: 0, complete: false, minLitres: 3 },
    sensors: { tapPulses: 0, drainPulses: 0, tapFlowing: false, drainFlowing: false, flowOnLpm: 1 },
    wifi: { rssi: -60, ip: '192.168.1.50' }, time: '2026-10-30 10:00', uptimeS: 600, outputs: 3, typicalS: [0, 0, 0],
    interrupted: { active: false, step: 0, epoch: 0 }, boot: 'Switched on or power cut', updatesAllowed: true,
    defaultPasswords: false, tankFault: false, screen: true, loop: { maxMs: 12, lastMinMaxMs: 8 },
    heap: { free: 180000, min: 150000 }, alert: { seq: 0, ok: false, code: 0 },
  };
}
let status = baseStatus();
let onStart = () => {};        // what the mock does when /api/start arrives
let onTestAlert = () => {};
let settings = { language: 'en', buzzerOn: false, minFillLitres: 3, flowOnLpm: 1, washPeriodS: 10, washOpenS: 2,
    drain2Min: 20, edgeS: 4, stepAlertMin: 90, tankMaxAgeH: 48, manualWashMaxMin: 10, allowRemoteStart: true,
    ntfyServer: 'https://ntfy.sh', ntfyTopic: 'x', timeZone: '<-05>5' };
let lastSettingsPost = null;
const server = http.createServer((req, res) => {
  const json = (code, obj) => { res.writeHead(code, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(obj)); };
  if (req.url === '/' ) { res.writeHead(200, { 'Content-Type': 'text/html' }); return res.end(PAGE); }
  if (req.url === '/api/status') return json(200, status);
  if (req.url === '/api/events') return json(200, []);
  if (req.url === '/api/settings' && req.method === 'POST') {
    let body = ''; req.on('data', c => body += c);
    return req.on('end', () => { lastSettingsPost = new URLSearchParams(body);
      const l = lastSettingsPost.get('language'); if (l) { settings.language = l; status.lang = l; } json(200, settings); });
  }
  if (req.url === '/api/settings') return json(200, settings);
  if (req.url === '/api/history') return json(200, {});
  if (req.url === '/api/start') { onStart(); return json(200, { ok: true }); }
  if (req.url === '/api/test-alert') { onTestAlert(); return json(200, { ok: true }); }
  if (req.url === '/api/stop') return json(200, { ok: true });
  json(404, { error: 'Not found.' });
});

// ── test runner ──
let passes = 0, failures = 0;
function check(cond, msg) { if (cond) passes++; else { failures++; console.log('  FAIL: ' + msg); } }
const toastText = (page) => page.locator('#toast').innerText();
// Advances the page's fake clock in 250 ms steps and lets real network replies arrive in between.
async function tick(page, ms) {
  for (let t = 0; t < ms; t += 250) { await page.clock.runFor(250); await new Promise(r => setTimeout(r, 15)); }
}

async function fresh(browser, base) {
  const ctx = await browser.newContext();
  const page = await ctx.newPage();
  await page.clock.install();
  await page.goto(base);
  await tick(page, 2500);
  return { ctx, page };
}

(async () => {
  await new Promise(r => server.listen(0, '127.0.0.1', r));
  const base = 'http://127.0.0.1:' + server.address().port + '/';
  const browser = await chromium.launch();

  // A1 Start is reported only after the controller's status shows the cycle running.
  {
    status = baseStatus(); onStart = () => {};
    const { ctx, page } = await fresh(browser, base);
    await page.click('#btnStart'); await page.click('#dlgOk');
    await tick(page, 1500);
    check(!(await toastText(page)).includes('Cycle started'), 'A1 "Cycle started" shown before the controller confirmed');
    status.state = 1; status.stateName = 'Starting';
    await tick(page, 4500);
    check((await toastText(page)).includes('Cycle started'), 'A1 "Cycle started" not shown after the controller confirmed');
    await ctx.close();
  }
  // A2 A start the controller never confirms gives a clear error after 10 s and re-enables Start.
  {
    status = baseStatus(); onStart = () => {};
    const { ctx, page } = await fresh(browser, base);
    await page.click('#btnStart'); await page.click('#dlgOk');
    await tick(page, 12000);
    const t = await toastText(page);
    check(t.includes('has not confirmed the start'), 'A2 no error after an unconfirmed start: "' + t + '"');
    check((await page.getAttribute('#btnStart', 'aria-disabled')) === 'false', 'A2 Start still disabled after the timeout');
    await ctx.close();
  }
  // A3 Float-switch disagreement is shown on the Overview and the Service tab.
  {
    status = baseStatus(); status.tank = 'high'; status.tankFault = true;
    const { ctx, page } = await fresh(browser, base);
    check((await page.innerText('#tankLevel')).includes('Check float switches'), 'A3 tank panel does not ask to check the floats');
    check((await page.innerText('#readyText')).includes('level switches disagree'), 'A3 ready text does not explain the fault');
    check((await page.innerText('#lvLow')) === 'Dry' && (await page.innerText('#lvHigh')) === 'Wet', 'A3 Service tab shows wrong switch states');
    await ctx.close();
  }
  // A4 A failed test alert is reported as failed, with the reason.
  {
    status = baseStatus(); onTestAlert = () => setTimeout(() => { status.alert = { seq: 1, ok: false, code: -2 }; }, 0);
    const { ctx, page } = await fresh(browser, base);
    await page.click('#tab-settings'); await tick(page, 500);
    await page.click('#btnTest'); await tick(page, 500);
    { const t0 = await toastText(page); check(t0.includes('Waiting for the alert server'), 'A4 test alert claimed as sent before delivery: "' + t0 + '"'); }
    await tick(page, 4500);
    const t = await toastText(page);
    check(t.includes('Test alert failed') && t.includes('no Wi-Fi'), 'A4 failure not reported: "' + t + '"');
    await ctx.close();
  }
  // A5 A delivered test alert is reported as delivered.
  {
    status = baseStatus(); onTestAlert = () => setTimeout(() => { status.alert = { seq: 1, ok: true, code: 200 }; }, 0);
    const { ctx, page } = await fresh(browser, base);
    await page.click('#tab-settings'); await tick(page, 500);
    await page.click('#btnTest'); await tick(page, 2500);   // success messages fade after 4 s
    check((await toastText(page)).includes('Test alert delivered'), 'A5 delivery not reported');
    await ctx.close();
  }
  // A6 Service tab shows loop time, memory, screen and last alert.
  {
    status = baseStatus(); status.screen = false; status.loop = { maxMs: 2345, lastMinMaxMs: 40 };
    const { ctx, page } = await fresh(browser, base);
    check((await page.innerText('#dLoop')).startsWith('2345 ms'), 'A6 loop time missing');
    check((await page.innerText('#dScreen')).startsWith('Not found'), 'A6 missing screen not shown');
    check((await page.innerText('#dHeap')).includes('KB free'), 'A6 memory missing');
    await ctx.close();
  }

  // A8 Spanish (the factory default): the page follows the controller's language,
  // and no English text is left on any tab.
  {
    status = baseStatus(); status.lang = 'es'; settings.language = 'es';
    const { ctx, page } = await fresh(browser, base);
    check((await page.innerText('#headline')) === 'Listo para el próximo lavado', 'A8 headline not in Spanish');
    check((await page.innerText('#tab-settings')).trim() === 'Configuración', 'A8 Settings tab not in Spanish');
    check((await page.getAttribute('html', 'lang')) === 'es', 'A8 page language not set to es');
    const english = Object.keys(ES).filter(k => ES[k] !== k && !k.includes('{'));
    for (const tab of ['overview', 'water', 'service', 'settings']) {
      await page.click('#tab-' + tab); await tick(page, 600);
      const left = await page.evaluate(keys => {
        const set = new Set(keys), found = [];
        const view = document.getElementById('view-' + document.querySelector('[aria-selected=true]').id.replace('tab-', ''));
        for (const root of [view, document.querySelector('header')]) {
          const w = document.createTreeWalker(root, NodeFilter.SHOW_TEXT); let n;
          while ((n = w.nextNode())) { const v = n.nodeValue.trim(); if (v && set.has(v) && n.parentElement.offsetParent !== null) found.push(v); }
        }
        return found;
      }, english);
      check(left.length === 0, 'A8 English text left on the ' + tab + ' tab: ' + left.join(' | '));
    }
    await ctx.close();
  }
  // A9 Choosing English in Settings sends language=en and switches the page.
  {
    status = baseStatus(); status.lang = 'es'; settings.language = 'es'; lastSettingsPost = null;
    const { ctx, page } = await fresh(browser, base);
    await page.click('#tab-settings'); await tick(page, 500);
    await page.selectOption('#f-language', 'en');
    await page.click('#btnSave'); await tick(page, 1000);
    check(lastSettingsPost && lastSettingsPost.get('language') === 'en', 'A9 language not sent with the settings');
    { const tx = (await page.innerText('#tab-settings')).trim(); check(tx === 'Settings', 'A9 page did not switch to English: ' + tx); }
    await ctx.close();
    settings.language = 'en';
  }

  // A7 Accessibility (axe-core, if installed): no violations on any tab, normal and fault states.
  let axePath = null;
  try { axePath = require.resolve('axe-core/axe.min.js'); } catch (e) { console.log('  (axe-core not installed: accessibility check skipped)'); }
  if (axePath) {
    for (const [fault, lang] of [[false, 'en'], [true, 'en'], [false, 'es'], [true, 'es']]) {
      status = baseStatus(); status.tankFault = fault; status.lang = lang; settings.language = lang; if (fault) status.tank = 'high';
      const { ctx, page } = await fresh(browser, base);
      await page.addScriptTag({ path: axePath });
      for (const tab of ['overview', 'water', 'service', 'settings']) {
        await page.click('#tab-' + tab); await tick(page, 500);
        const r = await page.evaluate(() => axe.run(document, { runOnly: ['wcag2a', 'wcag2aa'] }));
        check(r.violations.length === 0, 'A7 ' + tab + (fault ? ' (fault)' : '') + ' [' + lang + ']: ' +
          r.violations.map(v => v.id + ' x' + v.nodes.length).join(', '));
      }
      await ctx.close();
    }
    settings.language = 'en';
  }

  await browser.close(); server.close();
  console.log(passes + ' checks passed, ' + failures + ' failed');
  process.exit(failures ? 1 : 0);
})().catch(e => { console.error(e); process.exit(1); });
