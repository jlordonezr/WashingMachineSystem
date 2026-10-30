# Testing – firmware 3.2

**Status on 30 Oct 2026:** every automated suite passes and the firmware builds for the Arduino Nano ESP32 with cores 3.0.7 and 2.0.17. 3.0 has run correctly on the installed system. 3.1 adds languages only; 3.2 renames the app, adds the start-up animation and the Helvetica Bold screen font, and moves the buzzer to D1. Check H10–H12 on the real board.

## Automated (PC, no board)

Run everything: `sh tests/run_all.sh` (exit code 0 only if all pass).

| Suite | File | Checks | Covers |
|---|---|---|---|
| Flow meter, step gates, pump guard | `tests/test_flow.cpp` | 52 | Flow detection, noise, trickles, wrap-around, "machine is filled" guard, pump cut-off (since 2.4–2.6) |
| Float switches | `tests/test_level.cpp` | 19 | Debouncing towards safety, bobbing float, failed low float, fault timing, wrap-around (3.0) |
| Screen texts | `tools/check_screen_text.py` | 94 texts | Every screen text fits the OLED in both languages in Helvetica Bold 12 px, plain ASCII (3.1; font from 3.2) |
| Translations | `tools/i18n_keys.py` | 377 texts | Every app text has a Spanish translation (3.1) |
| Water history and settings storage | `tests/test_history.cpp` | 24 | Backup copy, unreadable records, migration, restore validation (since 2.8) |
| Full-cycle simulation | `tests/sim/` | 13 scenarios | The real firmware (all modules except Wi-Fi) against a simulated machine, tank, sensors, buttons and flash; the safety task is called every 20 ms of simulated time |
| Dashboard | `tests/app/test_dashboard.js` | 38 | The real page in headless Chromium against a mock controller; axe-core accessibility on all tabs (3.0) |
| Build | `arduino-cli compile --fqbn esp32:esp32:nano_nora` | — | 3.2.0: 1,368,305 bytes flash, 95,156 bytes RAM with core 3.3.12 (3.0.0: 1,301,277 / 72,624 with core 3.0.7) |

### Simulation scenarios

| ID | Scenario | Key checks |
|---|---|---|
| F1 | Full cycle reusing tank water | Step times, pump only while drawing, cut-off ≤ 0.35 s, never pump + tap together, statistics, alerts, event log |
| F2 | Drain pump at program start | Wash step does not end early |
| F3 | STOP during the wash | Ready, outputs safe, alert |
| F4 | Power cut during the rinse | Interruption detected after power-up, alert, screen, marker cleared |
| F5, F6 | Normal power-up; watchdog restart while Ready | No false interruption |
| F7 | Low float failed dry, tank full (3.0) | Fault alert; pump never runs; fill and wash valves never open into the full tank; cycle completes |
| F8 | High float bobbing at the mark (3.0) | Fill valve switches at most twice; tank stops below 39 L |
| F9 | No screen (3.0) | Runs, alerts, completes a cycle |
| F10 | `loop()` blocked 3 s as the machine closes its inlet (3.0) | Pump stops within 0.35 s anyway (measured 302 ms; **2.9.3: 3,200 ms**) |
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
| H8 | Manual tank wash | Ready with the low float wet, **Wash tank**, then lift the high float | Wash valve (D0) closes at once; screen `TANK CLEANED`; event *Manual tank wash ended: full* |
| H10 | Spanish screen (3.1) | Run one wash in Spanish; read every screen | Every text readable, nothing scrolls or is cut off; alerts arrive with accents |
| H9 | Setup page | Hold STOP 5 s at `READY`; join with the key shown; browse to `/update` | Key works; `/update` is not offered (setup page only) |
| H11 | Start-up screen (3.2) | Power on, watch the screen; then power on again and press START during the dog | Border Collie chases the butterfly (about 4 s), then `REUSO DE AGUA` / *Sistema listo* for 2 s, then `LISTO`; START ends the animation at once. Messages are in Helvetica Bold and readable |
| H12 | Buzzer on D1 (3.2) | Settings → Buzzer on; run a short cycle, or the float fault in H3 | Two short beeps at the end, three long beeps on the alarm. A short chirp at power-on is expected (D1 is the TX0 pin) |

Also: **a full real wash** (all four steps) with the tank half full, then check the *Water use* figures and the event log.

### Long-running stability

Leave the controller running for 7 days with at least one wash a day. Check daily on the Service tab: free memory (should not keep falling), the slowest loop pass in the last minute, and no restarts (*Last restart* and uptime). Note `millis()` wraps after 49.7 days; the logic is wrap-safe and covered by unit tests T10 and L7, but it has never been seen on the board.

## Results log

| Date | Version | Who | Result |
|---|---|---|---|
| 2026-10-30 | 3.2.0 | PC tests | All suites pass (browser tests skipped: no Node.js on this Mac) |
| — | 3.2.0 | Hardware H11, H12 | Not done yet |
| 2026-10-30 | 3.1.0 | PC tests | All suites pass (see `tests/test_results.txt`) |
| — | 3.1.0 | Hardware H10 | Not done yet |
| 2026-10-30 | 3.0.0 | Jose, installed system | Working as expected |
| 2026-10-30 | 3.0.0 | PC tests | All suites pass |
