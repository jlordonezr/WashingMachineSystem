# Testing – firmware 3.18

**Status on 30 Oct 2026:** every automated suite passes and the firmware builds for the Arduino Nano ESP32 with cores 3.0.7 and 2.0.17. 3.0 has run correctly on the installed system. 3.1 adds languages only; 3.2 renames the app, adds the start-up animation and the Helvetica Bold screen font, and moves the buzzer to D1. 3.3 pages the screen messages and makes phone alerts more reliable. Check H10–H13 on the real board.

## Automated (PC, no board)

Run everything: `sh tests/run_all.sh` (exit code 0 only if all pass).

| Suite | File | Checks | Covers |
|---|---|---|---|
| Flow meter, step gates, pump guard | `tests/test_flow.cpp` | 56 | Flow detection, noise, trickles, wrap-around, "machine is filled" guard, pump cut-off (since 2.4–2.6) |
| Sign-in tokens | `tests/test_auth.cpp` | 25 | SHA-256 (FIPS 180-2 vectors, one million `a`), HMAC-SHA256 (RFC 4231 cases 1, 2, 6), remember-device tokens: role and remember flag, refused after a password change, a new key, the viewer turned off or any changed character; the cookie header parser (3.11); what each role may do: the viewer the Overview controls only (3.15) |
| Float switches | `tests/test_level.cpp` | 19 | Debouncing towards safety, bobbing float, failed low float, fault timing, wrap-around (3.0) |
| Phone alerts | `tests/test_ntfy.cpp` | 31 | Topic rules, server address clean-up, JSON body (accents, quotes, `click`), UTF-8-safe cuts, which failures are retried (3.3), retry intervals and the 30 min window (3.3.1) |
| Estimated tank litres (3.13) | `tests/test_tank_volume.cpp` | 21 | Litres in and out, the wash valve at 6 L/min, 0-100 L limits, the four float corrections (and the exact value when a float changes), the start-up guess from the floats, the fill/drain trend |
| Screen texts | `tools/check_screen_text.py` | 173 texts and pages | Every screen text, and each page of a paged message, is plain ASCII and fits the 128 px line in both languages in all four screen fonts (3.1; pages from 3.3; one size per font from 3.9). From 3.12 (128x64 screen): at most two parts per message; headers at most 12 characters (3.13); at both baselines every glyph stays on the blue rows 17-63 and the two lines are at least 2 rows apart |
| Translations | `tools/i18n_keys.py` | 452 texts | Every app text has a Spanish translation (3.1); notes in `web/es.json` (keys starting with `_`, e.g. `_comment`) are not built into the page |
| Water history and settings storage | `tests/test_history.cpp` | 65 | Backup copy, unreadable records, migration, restore validation (since 2.8); settings version 7 → 8, the screen font and the minimum save flow (3.4); settings versions 10 and 11 → 12: the Large font of 3.14 becomes Hyperlegible, a chosen font is kept, the old default alert server moves to the new one and any other server is kept (3.15); cycle snapshot and the 3.9 marker (3.10); the sign-in key in flash (3.11); history version 3 → 4 with the last wash's mode unknown, a version 4 mode kept, a restore with mode 7 refused; a version 1 snapshot read as Reuse only, impossible modes not resumable (3.18) |
| Full-cycle simulation | `tests/sim/` | 43 runs (3.18) | The real firmware (all modules except Wi-Fi) against a simulated machine, tank, sensors, buttons and flash; the safety task is called every 20 ms of simulated time |
| Dashboard | `tests/app/test_dashboard.js` | 174 | The real page in headless Chromium against a mock controller; axe-core accessibility on all tabs (3.0); connection faults: a mock that hangs or drops the connection (3.16). Needs Node.js and Playwright; on this Mac a portable copy was used (`NODE_PATH` to its `node_modules`) |
| Web connections (3.16) | `tests/test_connpool.cpp` | 15 | `HttpConnPool.h`: a silent connection never holds up a request behind it (P1), silent ones closed after 5 s (P2), closed ones removed (P3), oldest request first (P4), full pool closes the oldest (P5), no copy kept after serving (P6), `millis()` wrap (P7) |
| Build | `arduino-cli compile --fqbn esp32:esp32:nano_nora` | — | 3.2.0: 1,368,305 bytes flash, 95,156 bytes RAM with core 3.3.12 (3.0.0: 1,301,277 / 72,624 with core 3.0.7) |

### Simulation scenarios

| ID | Scenario | Key checks |
|---|---|---|
| F1 | Full cycle reusing tank water | Step times, pump only while drawing, cut-off ≤ 0.35 s, never pump + tap together, statistics, alerts, event log; a second cycle shows *min left* (3.3) |
| F2 | Drain pump at program start | Wash step does not end early |
| F3 | STOP during the wash | Ready, outputs safe, alert |
| F4 | Power cut during the rinse | Interruption detected after power-up, alert, screen, marker cleared |
| F16 | *Reuse only* cycle cut after the machine filled in step 1, resumed from the app (3.10) | Flash holds the full snapshot during the cycle; after power-up Ready with every output off, Resume offered for 30 min, *Resume in app* on the screen; after Resume: step 1, *Reuse only* and the litres kept, step 1 ends on the wash drain at 600 s (not on the rinse drain), nothing saved to the tank, cycle completes, history includes the litres from before the cut, the resumed step is not used for the typical times, snapshot removed at the end |
| F17 | Resume limits (3.10) | A 3.9 marker is reported but not resumable; no Resume during a tank wash; none 30 min after power-up; none after dismiss; none for a snapshot 5 h old, offered for 1 h old; START replaces the offer and starts with no litres |
| F18 | 128x64 two-colour screen (3.12) | Screen started with 64 rows; header inside the yellow rows 0-15; the two pages of `READY` on two lines in the blue rows; three pages: the first stays on top, the others take turns under it and the notice lasts long enough for both; one page alone between the two lines; a new number in a countdown does not restart the pages |
| F19 | Screen at I2C address 0x3D (3.12) | Screen found and started at 0x3D, no missing-screen alert or event, `READY` drawn |
| F20 | Start-up and two-line messages (3.12) | No text during the start-up animation (25 frames per second) for 3.5 s; then `REUSE WATER` / *System ready* / *Version 3.12.0*; in a full cycle no message has more than two lines |
| F21 | Estimated tank litres (3.13) | From the floats at start (25 L between 10 and 40 L); during a whole cycle the estimate stays within 12 L of the simulated tank and ends within 3 L; the trend is draining while the pump draws and filling while step 4 saves; FULL and EMPTY in the header from the floats; the estimate survives a power cut (F21a, F21b) |
| F15 | START on the panel (3.17, replaces the 3.8 hold test) | A press during the start-up animation starts the cycle at once; at `READY` one press starts *Reuse and save* while START is still held; `REUSE & SAVE` › *Press START › to change*, then the step screen; step 4 saves |
| F22 | 2nd START press in step 1 (3.17) | *Reuse only* at once in the status, notice `REUSE ONLY`, event *Mode changed: reuse only (panel)*, snapshot in flash updated; step 4 *Not saving*, nothing saved |
| F23 | Press in step 1, 3rd and 4th press in step 3 (3.17, 3.18) | 3rd: *Reuse all*; 4th: back to *Reuse and save*, event *Mode changed: reuse and save (panel)*, 3 switches logged, snapshot updated; step 4 saves |
| F24 | START press in step 4 (3.17) | Mode and step unchanged, *Mode fixed › Step 4 running*, no event; still saving |
| F25 | Bounce at `READY` (3.17) | Two press edges 50 ms apart: one start, no mode change (fails with `START_MIN_GAP_MS` 0); a real press 2 s later switches |
| F26 | START held at power-up, or stuck in a cycle (3.17) | Held from power-up for 25 s: no cycle, *Button stuck*, *START held 15 s: treated as stuck*; release starts nothing; a later press starts. A press that sticks for 20 s: one start, no mode change, reported stuck |
| F27 | Switch to *Reuse only*, power cut, resume (3.17) | Snapshot `noSave` 1 (F27a); after power-up the cut cycle is *Reuse only*, the resumed cycle too, nothing saved (F27b) |
| F28 | App starts *Reuse only*, START twice in step 2 (3.17, 3.18) | *Reuse all*, then *Reuse and save*, both logged; step 4 saves |
| F29 | STOP after a switch (3.17) | Ready; pump, fill and wash outputs off; *Cycle stopped*; snapshot removed |
| F30 | *Reuse all*, full cycle from 30 L (3.18) | Pump on in step 1 and step 3; *Reusing water* under `RINSE 3 OF 4`; tank ends at the low float (10 L); fill valve never on, nothing saved, *Not saving* in step 4, no tank-full alert; reused more than a *Reuse only* cycle from the same 30 L (measured 23.7 L vs 15.8 L); start and step 3 in the event log; `last.mode` 2 |
| F31 | *Reuse all*, the tank reaches the low float mid-rinse (3.18) | Step 3 changes to tap during the draw, *Tank is low* shown, pump and tap never on together, dead-heading ≤ 350 ms, tank not pumped below 9 L |
| F32 | *Reuse all* with 5 L in the tank (3.18) | Pump never on, whole cycle on tap, nothing reused or saved, *Tank is low* shown |
| F33 | *Reuse and save* and *Reuse only* (3.18 regression) | Pump in step 1 only, never in step 3; step 4 as before |
| F34 | START presses 1–4, then a press in step 4 (3.18) | *Reuse and save*, *Reuse only*, *Reuse all* (`REUSE ALL` › *Press START › to change*, snapshot mode 2), *Reuse and save*; step 4 unchanged, *Mode fixed*; 3 switches logged; step 4 saves |
| F35 | Switch to *Reuse all* during a step 3 draw, then away (3.18) | The pump starts on the current draw and the tank goes down; switching away goes back to tap at once; never pump and tap together |
| F36 | Power cut in step 3 while *Reuse all* pumps, resume (3.18) | Snapshot version 2, mode 2 (F36a); after power-up pump off, reported as *Reuse all*, the resumed step 3 pumps on the current draw, nothing saved, `last.mode` 2 (F36b) |
| F37 | Snapshot from 3.10–3.17 (version 1) at power-up (3.18) | noSave 0 read as *Reuse and save*, 1 as *Reuse only*; resumable; resumed step 3 on tap |
| F38 | STOP in step 3 while *Reuse all* pumps (3.18) | Ready; pump, fill and wash off; tap and drain normal; snapshot removed |
| F5, F6 | Normal power-up; watchdog restart while Ready | No false interruption |
| F7 | Low float failed dry, tank full (3.0) | Fault alert; pump never runs; fill and wash valves never open into the full tank; cycle completes |
| F8 | High float bobbing at the mark (3.0) | Fill valve switches at most twice; tank stops below 39 L |
| F9 | No screen (3.0) | Runs, alerts, completes a cycle |
| F10 | `loop()` blocked 3 s as the machine closes its inlet (3.0) | Pump stops within 0.35 s anyway (measured 302 ms; **2.9.3: 3,200 ms**) |
| F13 | Step 4 with a trickle and a second pump-out (3.4) | 0.3 L/min goes to the drain (fill closed, drain open within 3 s), none of it into the tank; a 12 L/min pump-out is saved again 2–6 s after it starts; events logged; *Low flow* on the screen; no tank-full alert |
| F12 | Full cycle in Spanish (3.1) | Spanish screen texts in every step, `TERMINADO` / *Reusados n L*, Spanish alert title, event log stays English |
| F11 | `loop()` blocked 3 s while the tank fills in step 4 (3.0) | Fill valve closes; tank 40.06 L (**2.9.3: overfilled to 40.57 L**) |

### Dashboard checks

| ID | Check |
|---|---|
| A1 | *Cycle started* appears only after the status shows the cycle running |
| A2 | An unconfirmed start gives an error after 10 s and re-enables Start |
| A3 | A float-switch fault is shown on the Overview and the Service tab |
| A4, A5 | Test alert: failure reported with its reason; delivery reported |
| A6 | Service tab shows loop time, memory and a missing screen |
| A7 | axe-core (WCAG 2 A/AA): 0 violations on all four tabs, normal and fault states, in English and Spanish |
| A8 | Spanish: the page follows the controller's language and no English text is left on any tab (3.1) |
| A9 | Choosing English in Settings sends `language=en` and switches the page (3.1) |
| A10 | Viewer user: only Overview and Water use (3.5); Start, Reuse only, Stop and Wash tank are shown (3.15); no Restore from file; the note is shown, arrow keys skip the hidden tabs |
| A11 | A page whose status turns to `viewer` leaves the Settings tab; back to `admin` restores the controls (3.5) |
| A12 | Turning the viewer on without a password is refused on the page; with one, `viewerOn=1` and the password are sent (3.5) |
| A13 | One mode control and one Start (3.18.1): legend *Cycle mode*, 3 radios, Reuse and save by default, the old buttons gone; for each mode the dialog title and text, `mode=0/1/2` sent, *Starting…* and the bar locked while pending; the Reuse only note and *Final drain*; an older status with only `noSave` shows Reuse only |
| A14 | Resume popup (3.10): shown once after a cut, names the step, *Reuse only* and the minutes left; *Not now* keeps a Resume button on the notice; Resume (popup or notice) calls `/api/interrupted/resume`; *Don't resume* dismisses; closes when the offer expires; offered to the viewer too (3.15); not shown with *Start from the dashboard* off; Spanish |
| A15 | Sign-in form (3.11): only the form while the controller answers 401; *Remember this device* on and user `admin` by default; a wrong password is reported; the right one sends `remember=1` and shows the app; *Sign out other devices* sends `all=1` and keeps this device signed in; *Sign out* returns to the form and the badge reads *Signed out* (no Offline) |
| A16 | Online → *Reconnecting…* after one failure → *Offline* after three → Online again; the last data stays, greyed out, with its age and the STOP advice; no toast at any time; retries back off (3.16) |
| A17 | One status request at a time: Refresh while one hangs starts no second one and shows *Refreshing…*; after the 4 s limit exactly one retry (3.16) |
| A18 | Hidden page (phone locked): no requests and no error for 20 s; shown again: one request at once (3.16) |
| A19 | On a phone-sized screen an error message is in the page (not floating) and covers neither Stop nor its help; a "can't reach" error clears when the controller answers again (3.16) |
| A22 | *Reuse all* (3.18): offered at Ready; `cycle.mode` 2 shows the *Reuse all* note and names step 4 *Final drain*, mode 1 the *Reuse only* note; Spanish *Reusar todo* on the button and in the note. A13 also checks the third button and that the buttons send `mode=0/1/2` |
| A23 | Mode control by keyboard (3.18.1): arrow keys move the choice, the description follows, a visible focus ring, `aria-describedby`; the choice is kept after a reload; stored `7`, `x` or empty fall back to Reuse and save |
| A24 | When the mode and Start can be used (3.18.1): remote start off → Start disabled, mode still changeable, hint shown; during a cycle Start and the bar disabled, the bar shows `cycle.mode` and follows two switches mid-cycle; back at Ready the remembered choice returns and was not overwritten; a poll at Ready keeps the person's choice |
| A25 | 320 px wide, English and Spanish (3.18.1): the three segments on one row, no horizontal overflow, segments and Start at least 44 px; Spanish legend, labels, Start and description |
| A21 | A mode switch from START on the panel (3.17): the *Reuse only* note and the step 4 name (*Save* / *Final drain*) follow `cycle.noSave` in the next status, without a reload |
| A20 | A Stop with no answer is sent once, the app says it is checking, and confirms it from the status; a request that times out after the status confirmed it shows no error (3.16) |

## What the PC tests cannot show

- Real SSR switching, real flow-sensor signals, real float switches and their wiring.
- Real timing on the ESP32: FreeRTOS scheduling, Wi-Fi load, flash writes, I2C.
- How a real phone on a weak signal loads the page.
- Long-term effects: heap fragmentation, NVS wear, days of uptime.

## Hardware checks (manual, on the installed system)

Do these with the washing machine empty and someone at the controller. Note each result and the Service tab values.

| ID | Check | How | Pass |
|---|---|---|---|
| H1 | Boot state | Power on. Watch all five SSR LEDs | All off at power-up, then tap + drain on at `READY` |
| H2 | Float switches | Lift each float by hand, one at a time, while watching the Service tab | Low: Dry/Wet follows within 2 s; High: Wet within 0.2 s, Dry 2 s after release |
| H3 | Float fault | Hold the high float up with the low float down for 5 s | Alert *check level switches*, app shows *Check float switches*, alarm beeps if the buzzer is on |
| H4 | Interlock in step 4 | Start a cycle, run to step 4 (or use a short program), then lift the high float | Fill valve (D5) off within about 0.3 s, drain valve on; stays off when released |
| H5 | Loop time under load | On a phone at the edge of Wi-Fi range, reload the app 10 times during a cycle | Service tab: slowest pass noted. Any value is acceptable for safety in 3.0, but over 1 s shows in the event log |
| H6 | Pump cut-off | Step 1 with tank water: when the machine closes its inlet, time the pump | Pump off within about 0.3 s (listen or watch the SSR LED); event *Pump stopped: machine closed its inlet* |
| H7 | Pump cut-off while the page loads | Repeat H6 while a weak-signal phone reloads the app | Same as H6 |
| H19 | Resume after a power cut (3.10) | Start a short wash with *Solo reusar*; once the machine has filled in step 1, unplug the controller for 30 s (leave the washer on), plug it in, open the app | Outputs off at power-up; screen `INTERRUMPIDO` › *Reanude en app*; the app shows *¿Reanudar el ciclo interrumpido?* with step 1 and *Solo reusar*. *Reanudar ciclo*: `REANUDADO`, step 1 continues and ends when the washer drains; step 4 sends the rinse to the drain. Repeat and wait 30 min: the popup closes and only *Listo, revisé la lavadora* remains |
| H20 | Sign in once (3.11) | On a phone: open the app, sign in with *Recordar este dispositivo* on. Close the browser completely (and restart the phone), open the app again. Then, on a second phone, sign in; on the first, Settings → *Cerrar sesión en otros dispositivos* | Asked once only; after the restart the app opens straight to Overview. The second phone shows the sign-in form at its next refresh; the first stays signed in. The browser's own password box never appears |
| H21 | New 128x64 screen and messages (3.12) | Fit the yellow/blue screen (labels on the module: VCC → 3V3, GND → GND, SDA → A4, SCL → A5), flash 3.12, power on. Run one wash. Open Service → Loop time | Animation: the collie gallops along the bottom of the blue rows and the butterfly flutters up into the yellow band, turning yellow; nothing looks broken where it crosses the gap between yellow and blue. `LISTO` and the two icons in yellow; *Inicie lavadora* and *y pulse START* on two blue lines, nothing cut off at the yellow/blue gap. Every message is on two lines (or one, centred) and stays still; nothing is cut off. Service shows the screen as working; the longest loop pass stays under 100 ms |
| H22 | Tank litres (3.13) | Note the litres in the header. Run a wash with tank water; watch the header in step 1 and in step 4. Then check the tank with a ruler or by eye | Step 1: the litres fall and the arrow points down while the pump runs; step 4: they rise and the arrow points up; `FULL` when the high float is wet. The estimate is close to the real tank (within about 10 L); if not, measure where the floats sit and set `TANK_LOW_MARK_L` / `TANK_HIGH_MARK_L` |
| H23 | App during a wash (3.16) | On the iPhone, at `http://reusewater.local`: start a wash and keep the app open on Overview through all four steps. Lock the phone for 2 min in step 1 and in step 3, unlock and return to Safari. Meanwhile on a computer: `python3 tools/latency_probe.py --minutes 70 --idle 2` | Badge stays *Online* (at most a brief *Updating…* after unlocking, no error message); probe: name lookup under 1 s, no answer over 300 ms, 0 failed; Service → *App response time* under 300 ms |
| H8 | Manual tank wash | Ready with the low float wet, **Wash tank**, then lift the high float | Wash valve (D0) closes at once; screen `TANK CLEANED`; event *Manual tank wash ended: full* |
| H10 | Spanish screen (3.1) | Run one wash in Spanish; read every screen | Every text readable, nothing scrolls or is cut off; alerts arrive with accents |
| H9 | Setup page | Hold STOP 5 s at `READY`; join with the key shown; browse to `/update` | Key works; `/update` is not offered (setup page only) |
| H11 | Start-up screen (3.2) | Power on, watch the screen; then power on again and press START during the dog | Border Collie chases the butterfly (about 4 s), then `REUSO DE AGUA` / *Sistema listo* for 2 s, then `LISTO`; START ends the animation at once. Messages are in Helvetica Bold and readable | (3.12: the collie runs along the bottom, the butterfly flutters up into the yellow band)
| H15 | Minimum flow to save (3.4) | In step 4, watch Service → Valves and pump as the rinse pump-out ends | Fill valve turns off and drain on within about 3 s of the flow dropping below the limit; screen *Flujo bajo › Al desague*; a spin pump-out turns the fill valve on again |
| H16 | Viewer user (3.5, Overview controls from 3.15) | Settings → *Viewer user* on, set a password, Save. On another phone (or a private browser window) sign in as `viewer`; start a wash, stop it, start and stop a tank wash | Only Overview and Water use; Reuse and save, Reuse only, Stop cycle and Wash tank work; no Service, Settings or history restore. As admin again, turning it off makes the viewer password stop working |
| H18 | START press and mode switch (3.17, 3.18) | At `LISTO` (buzzer on): press START once; in step 1 press it again, and again; in step 2 press it again; in step 4 press it again. Then a new wash: press START twice quickly (under 1 s). Then switch off, hold START, switch on and keep holding 20 s | First press: the cycle starts at once, short beep, `REUSO+AHORRO` › *Pulse START › para cambiar* for about 3 s, then `LAVADO 1/4`. Step 1: `SOLO REUSAR`, then `REUSAR TODO`; the app shows the matching note. Step 2: `REUSO+AHORRO`. Step 4: *Modo fijo › Paso 4 en curso*, still `AHORRO 4/4`, saving. Quick double press: starts *Reusar y guardar* only. Held at power-up: nothing starts, `BOTON START` › *Boton atascado*. Service → event log lists each *Mode changed* |
| H24 | Reuse all (3.18) | With the tank about half full, start the machine, then *Reusar todo* in the app; watch Service → Valves and pump in step 1 and step 3; let the tank reach the low float during the rinse if possible | Step 1 and step 3 *Reusando agua*, pump on only while the machine draws; when the low float drops, *Agua de la llave › Tanque bajo* and the tap valve opens within about a second, pump off; step 4 `DESAGUE 4/4` *Sin guardar*, fill valve off; Water use → last wash: mode *Reusar todo*, reused litres include the rinse |
| H17 | Reuse only (3.7) | With water in the tank, start the machine, then *Solo reusar* in the app; watch step 1 and step 4 | Step 1 *Reusando agua*; step 4 header `DESAGUE 4 DE 4`, *Sin guardar › Al desague*; Service → Valves: fill valve off, drain on all through step 4; *TERMINADO* shows only the litres reused. Then press START on the controller: a normal cycle that saves |
| H14 | Screen font (3.9) | Settings → Screen font: choose each of the four styles (Hyperlegible, Plain sans, Terminal bold, Schoolbook bold), Save, look at the screen during a wash | The screen changes at once; in each style every message has the same letter size, nothing scrolls or is cut off, the letters sit centred under the line; the choice is kept after Restart controller |
| H13 | Screen pages (3.3) | Run one wash in Spanish; watch each step for 15 s | Pages change every 3 s and each is readable; header `LAVADO 1 DE 4` … `AHORRO 4 DE 4`; *Faltan n min* from the second wash and roughly right; `TERMINADO` shows *Lavado listo › Reusados n L › Guardados n L* |
| H12 | Buzzer on D1 (3.2) | Settings → Buzzer on; run a short cycle, or the float fault in H3 | Two short beeps at the end, three long beeps on the alarm. A short chirp at power-on is expected (D1 is the TX0 pin) |

Also: **a full real wash** (all four steps) with the tank half full, then check the *Water use* figures and the event log.

### Long-running stability

Leave the controller running for 7 days with at least one wash a day. Check daily on the Service tab: free memory (should not keep falling), the slowest loop pass in the last minute, and no restarts (*Last restart* and uptime). Note `millis()` wraps after 49.7 days; the logic is wrap-safe and covered by unit tests T10 and L7, but it has never been seen on the board.

## Results log

| Date | Version | Who | Result |
|---|---|---|---|
| 2026-10-30 | 3.2.0 | PC tests | All suites pass (browser tests skipped: no Node.js on this Mac) |
| — | 3.2.0 | Hardware H11, H12 | Not done yet |
| 2026-11-03 | 3.3.0 | PC tests, firmware build | All suites pass (browser tests skipped: Playwright not installed); 1,372,029 bytes flash, 95,916 bytes RAM with core 3.3.12 |
| — | 3.3.0 | Hardware H13, test alert | Not done yet |
| 2026-11-04 | 3.4.1 | PC tests, firmware build | All suites pass (browser tests skipped: Playwright not installed); 1,383,837 bytes flash with core 3.3.12 |
| — | 3.4.1 | Hardware H14, H15 | Not done yet |
| 2026-11-04 | 3.5.0 | PC tests, firmware build | All suites pass (browser tests A10–A12 written, skipped: no Node.js on this Mac); 1,385,957 bytes flash, 99,732 bytes RAM with core 3.3.12 |
| — | 3.5.0 | Hardware H16 | Not done yet |
| 2026-11-04 | 3.6.0 | PC tests, firmware build | All suites pass (browser tests skipped: no Node.js on this Mac); 1,390,025 bytes flash, 99,732 bytes RAM with core 3.3.12 |
| — | 3.6.0 | Hardware H14 (four new fonts) | Not done yet |
| 2026-11-06 | 3.7.0 | PC tests, firmware build | All suites pass, simulation F14 included (browser test A13 written, skipped: no Node.js on this Mac); 1,392,069 bytes flash, 99,740 bytes RAM with core 3.3.12 |
| — | 3.7.0 | Hardware H17 (Reuse only) | Not done yet |
| 2026-11-06 | 3.8.0 | PC tests, firmware build | All suites pass, simulation F15 included (browser tests skipped: no Node.js on this Mac); 1,392,649 bytes flash, 99,740 bytes RAM with core 3.3.12 |
| — | 3.8.0 | Hardware H17, H18 | Not done yet |
| 2026-11-06 | 3.9.0 | PC tests, firmware build, rendered check | All suites pass (browser tests skipped: no Node.js on this Mac); all 83 message pages drawn in the 4 fonts as the firmware does: one size, none clipped; 1,383,001 bytes flash, 99,740 bytes RAM with core 3.3.12 |
| — | 3.9.0 | Hardware H14 (fixed sizes) | Not done yet |
| 2026-11-06 | 3.10.0 | PC tests, firmware build | All suites pass, simulation F16, F17, unit S6b, C1–C3 included (browser test A14 written, skipped: no Node.js on this Mac; page script syntax-checked); 1,386,013 bytes flash with core 3.3.12 |
| — | 3.10.0 | Hardware H19 (resume) | Not done yet |
| 2026-11-06 | 3.11.0 | PC tests, firmware build, headless Chrome | All suites pass, `test_auth` and K1 included (browser test A15 written, skipped: no Node.js on this Mac). Sign-in checked in headless Chrome 6 steps with a persistent profile against a mock controller: form only; wrong password reported; signed in; still signed in after the browser restarted; Sign out; without Remember, signed out after the restart. 1,391,345 bytes flash with core 3.3.12 |
| — | 3.11.0 | Hardware H20 (sign in once) | Not done yet |
| 2026-11-13 | 3.12.0 | PC tests, firmware build | All suites pass, simulation F18, F19, F20 and the two-line checks included (173 screen texts and pages; browser tests skipped: no Node.js on this Mac); 1,392,049 bytes flash with core 3.3.12 |
| — | 3.12.0 | Hardware H21 (new 128x64 screen) | Not done yet |
| 2026-11-13 | 3.13.0 | PC tests, firmware build | All suites pass, `test_tank_volume` and simulation F21 included (177 screen texts and pages; browser tests skipped: no Node.js on this Mac) |
| — | 3.13.0 | Hardware H22 (tank litres) | Not done yet |
| 2026-11-15 | 3.14.0 | PC tests, firmware build, upload | All suites passed; uploaded to the installed controller over Wi-Fi. Large screen font; withdrawn in 3.15 |
| 2026-11-15 | 3.15.0 | PC tests, firmware build | All suites pass, `test_history` S10 and S11 and `test_auth` R1 included (177 screen texts and pages; browser tests skipped: no Node.js on this Mac, so the changed A10 and A14 have not run) |
| — | 3.15.0 | Hardware: screen back to two lines in Hyperlegible; test alert through `https://ntfy.hostux.net`; H16 with the viewer starting and stopping | Not done yet |
| 2026-11-20 | 3.15.0 | Measured on the installed controller, from a Mac on the home Wi-Fi | Status answer median 24 ms, slowest 308 ms; with one silent connection open **4.1 s every time**; `reusewater.local` lookup **5.0 s every time** (no IPv6 answer). Cause of the app's timeouts during washes |
| 2026-11-20 | 3.16.0 | PC tests, firmware build | All suites pass, `test_connpool` P1–P7 and browser tests A1–A20 with axe-core (112 checks, first run on this Mac; existing A13/A15 assumptions corrected, A12 and A14 found two page bugs) |
| 2026-11-20 | 3.16.0 | Upload over Wi-Fi (espota.py), measured from a Mac | Back up in 3 s; `/api/ping` answers. `reusewater.local` now also has its IPv6 address: lookup 4–6 ms (5.0 s on 3.15). Status/ping answers: median 23 ms, slowest 44 ms; with 2 silent connections held open median 22 ms, slowest 70 ms (4.1 s every time on 3.15); 0 over 300 ms, 0 failed |
| — | 3.16.0 | Hardware H23 (app during a wash, iPhone, lock and unlock) | Not done yet |
| 2026-10-30 | 3.1.0 | PC tests | All suites pass (see `tests/test_results.txt`) |
| — | 3.1.0 | Hardware H10 | Not done yet |
| 2026-10-30 | 3.0.0 | Jose, installed system | Working as expected |
| 2026-10-30 | 3.0.0 | PC tests | All suites pass |
| 2026-11-25 | 3.17.0 | PC tests, firmware build | All suites pass, simulation F15 (rewritten) and F22–F29, browser tests A1–A21 with axe-core (115 checks); 1,396,929 bytes flash, 100,276 bytes RAM with core 3.3.12 |
| 2026-11-25 | 3.17.0 | Upload over Wi-Fi (espota.py) to 192.168.40.30 | Accepted; back up within seconds (`/api/ping` uptime 8 s), app page served |
| — | 3.17.0 | Hardware H18 (START press and mode switch) | Not done yet |
| 2026-11-25 | 3.18.0 | PC tests, firmware build | All suites pass: simulation F30–F38 (F23, F28 updated for three modes), history H4c and C1b, browser tests A1–A22 (124 checks); with step 3 forced to tap water F30, F31, F35 and F36 fail; 1,399,377 bytes flash, 100,276 bytes RAM with core 3.3.12 |
| — | 3.18.0 | Hardware H18 (three modes), H24 (Reuse all) | Not done yet |
| 2026-11-25 | 3.18.1 | PC tests, firmware build | All suites pass, browser tests A1–A25 (174 checks); the mode bar checked by eye at 320 px in light and dark (75×44 px segments); 1,400,017 bytes flash, 100,276 bytes RAM with core 3.3.12 |
| 2026-11-25 | 3.18.1 | Upload over Wi-Fi (espota.py) to 192.168.40.30 | Accepted; back up within seconds (`/api/ping` uptime 11 s); the served page has the Cycle mode bar and Start cycle, no old start buttons |
| — | 3.18.1 | Hardware H18 (three modes), H24 (Reuse all) | Not done yet |
