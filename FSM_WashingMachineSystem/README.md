# Reuse Water System – Firmware 3.2 (Wi-Fi)

## What's new in 3.2: new name, start-up animation, clearer screen, buzzer on D1

The wash cycle, the safety logic, the settings and the water history are unchanged from 3.1. Settings and history formats are the same (versions 7 and 3): no reset, no migration.

- **New name: Reuse Water System** (*Sistema de Reúso de Agua*): app title, screen, alert titles (*Reuse Water: …* / *Reúso de Agua: …*), sign-in prompt and setup page. The app moves to `http://reusewater.local`, the Wi-Fi setup network is `ReuseWater-Setup` and the wireless-update port is *reusewater*. Internal names stay (`greywater` storage namespace, history file format, default password, source file names), so settings, history and backups are kept.
- **Start-up animation:** after power-on a Border Collie chases a butterfly across the whole screen (about 4 s), then *System ready* shows for 2 s as before. Notices that arrive meanwhile (Wi-Fi address, setup key, *Interrupted*) are shown after it; START or a tank wash ends it at once (`BootAnimation.h`, about 1.6 KB).
- **Clearer screen font:** messages use the Helvetica Bold pixel font, 14 px when the message fits and 12 px otherwise (`FontHelvBold14.h`, `FontHelvBold12.h`). Every screen text fits in 12 px; `tools/check_screen_text.py` measures with the new font. The header keeps the built-in font.
- **Larger tank icon** in the screen header (17 × 10 px, with half-way marks).
- **Buzzer moved from D12 to D1 (TX0).** Do not use `Serial0`. The chip's start-up messages go out on TX0 before the firmware runs, so the buzzer may chirp briefly at power-on; `setup()` now drives the pin LOW first thing.
- **Runbook and Quick Card** rebuilt from editable sources in `manuals/src/` (see `manuals/README.md`) and updated for 3.2.
- Build: 1.37 MB flash (43%), 95 KB RAM (29%) with Espressif core 3.3.12.

## What's new in 3.1: Spanish

The controller screen, the app and the phone alerts are now in **Spanish (Colombia, usted)** or English, chosen in the app under **Configuración → Idioma / Settings → Language**. Spanish is the default, also after updating from 3.0. The wash cycle, the safety logic and the wiring are unchanged from 3.0.

- **Screen:** plain-ASCII Spanish, because the screen fonts have no accents or ñ: `LISTO` / *Pulse START*, `1/4 LAVADO`, `2/4 DESAGUE`, `3/4 ENJUAGUE`, `4/4 AHORRO`, *Reusando agua*, *Agua de la llave*, *Guardando*, `TERMINADO`. Every screen text in both languages is measured against the real font (`tools/check_screen_text.py`): headers fit 16 characters and messages 128 px, so nothing scrolls. Firmware code marks screen texts with `TRS("English", "Español")` and other texts with `TR(...)`.
- **App:** every text (377) has a Spanish translation in `web/es.json`. `tools/build_dashboard.py` puts it into the page; `tools/i18n_keys.py` lists any text without one. Dates and numbers use the Colombian format. Error messages from the controller are translated by the app.
- **Alerts:** titles and messages in the chosen language. They are now sent as JSON to the ntfy server's root URL (ntfy "publish as JSON"), so accents arrive intact; plain HTTP headers cannot carry them.
- **Stays in English:** the event log on the Service tab (for service and support), and the Wi-Fi setup page (it comes from the WiFiManager library).
- **Setup key on the screen:** in Spanish the screen shows only the key (`gw-a1b2c3`), because *Clave gw-…* does not fit.
- **Settings version 7** adds the language; version 6 settings from 3.0 are kept and start in Spanish. Water history is unchanged.
- **Tests:** simulation F12 runs a full cycle in Spanish; history test S1–S2 checks the settings upgrade; dashboard tests A8 (no English left on any tab in Spanish) and A9 (switching language), and axe-core in both languages; screen-text and translation checks in `tests/run_all.sh`.
- Build: 1.31 MB flash (41%) with core 3.0.7, 1.13 MB with core 2.0.17.


## What's new in 3.0

3.0 hardens 2.9 after a full technical assessment. The wash cycle, the pins and the wiring are unchanged. 3.0 passed every PC test and simulation and has run correctly on the installed system. 2.9.3 (same fixes, without the safety task) is kept as a fallback.

### Safety: valves and pump no longer depend on the web server (finding F1)

In 2.9 every valve and pump decision ran in `loop()`, together with the web server. A phone on weak Wi-Fi could hold `loop()` for seconds (the ESP32 web server waits up to 5 s per unacknowledged chunk). During that time the 300 ms pump cut-off and the tank-full checks did not run. In the simulation, a 3 s stall made 2.9.3 run the pump against a closed inlet for 3.2 s and overfill the tank; 3.0 stops the pump in 0.3 s and holds the tank at the high mark.

- **Safety task** (`GreyWaterTask.cpp`): runs every 20 ms at a higher priority than `loop()`. It reads the float switches, runs the pump cut-off and enforces the interlocks, whatever `loop()` is doing.
- **One owner for the outputs** (`GreyWaterOutputs.cpp`): only this module writes the five SSR pins, behind one lock. The cycle *requests* a mode; the interlocks may override it:
  - fill valve (rinse water to the tank) never open while the high float is wet,
  - tank-wash valve never open while the high float is wet (also during the tank-water fill in step 1),
  - pump never on while the low float is dry.
- The interlocks fall back to the same safe states 2.9 used: tap fill instead of the pump, "not saving" (drain open) instead of saving.

### Safety: float switches (findings F2, F3)

- **Overflow fixed:** 2.9 read "high float wet, low float dry" as an *empty* tank, so step 4 kept filling a tank that was full. Now the tank is full whenever the high float is wet. The pump is used only when the low float is wet.
- **Disagreement is reported:** after 3 s of "high wet, low dry", the controller logs it, sounds the alarm beeps and sends a high-priority alert. The app shows *Check float switches*.
- **Debounced, towards safety:** each switch accepts the safe reading in 0.2 s (high float wet, low float dry) and the other reading only after 2 s steady (`TankLevel.h`). A float bobbing at the high mark no longer switches the fill valve every 250 ms; step 4 stops saving for good once the tank is full.

### Reliability

- **No screen, no stop (F8):** 2.9 halted in `setup()` if the OLED did not answer. Now the controller runs without it, logs it and sends an alert.
- **Loop timing and memory** on the Service tab: the slowest `loop()` pass since start-up and in the last minute, and free memory. Passes over 1 s are logged.
- **Smaller page:** the dashboard is sent gzip-compressed, 22 KB instead of 80 KB, so serving it holds the web server for about a quarter of the time. Edit `web/dashboard.html`, then run `python3 tools/build_dashboard.py`.

### Security

- **Setup page (F4):** the Wi-Fi setup page no longer offers firmware upload or erase. WiFiManager registers those pages without a password; 3.0 answers them with "not available" first. The setup network's password is now unique to each board (`gw-` + six characters), shown on the screen while the page is open and printed over USB. Without a working screen it falls back to `greywater`.
- **Update password (F5):** a new update password works at once; 2.9 kept the old one until a restart.
- **Lockout per device (F7):** 5 wrong passwords lock out only the device that sent them, for a minute. In 2.9 anyone on the network could lock everyone out, including the remote Stop.
- **Input checks (F10):** settings numbers are clamped before conversion. **Stop while Ready (F11)** now answers `409 No cycle is running` instead of OK.

### The app reports only what the controller confirmed (F6)

- *Cycle started*, *Cycle stopped* and *Tank wash started* appear only when the controller's status shows the change. If it does not within 10 s, the app says so and re-enables the buttons.
- *Send a test alert* now reports whether the alert server accepted it (*Test alert delivered* or *Test alert failed (no Wi-Fi)*). The Service tab shows the last alert's result; every delivery or failure is in the event log.

### Engineering

- `GreyWaterSystem.cpp` (978 lines) is split by concern: cycle (`GreyWaterSystem.cpp`), outputs and interlocks (`GreyWaterOutputs.cpp`), safety task (`GreyWaterTask.cpp`), flow sensors (`GreyWaterSensors.cpp`), screen (`GreyWaterDisplay.cpp`), event log, buzzer and timing (`GreyWaterDiagnostics.cpp`). Function names used by other files are unchanged.
- **Tests:** 19 float-switch checks (`tests/test_level.cpp`); simulation scenarios F7–F11 (failed float, bobbing float, no screen, `loop()` stalled during pumping and during saving); 21 dashboard checks in a headless browser, including axe-core accessibility on every tab (`tests/app`). `tests/run_all.sh` now fails if any suite fails, and the test stubs build on Linux.
- New status fields: `tankFault`, `screen`, `loop`, `heap`, `alert`. See `docs/PROTOCOL.md`.
- Build: 1.30 MB flash (41%), 72.6 KB RAM (22%), Espressif core 3.0.7. Settings and history formats are unchanged (versions 6 and 3): no reset, no migration.

### Documentation

`docs/ARCHITECTURE.md`, `docs/PROTOCOL.md`, `docs/INSTALL.md`, `docs/TESTING.md` (with the hardware test plan) and the known limitations below.

### Known limitations and pending

- 3.1's Spanish texts have been checked in the simulation, the browser tests and the screen-width check; check them once on the real screen.
- The dashboard still uses HTTP Basic over plain HTTP; keep it on the home network. Passwords are stored unencrypted in flash.
- `/restart` on the Wi-Fi setup page remains (WiFiManager); the interrupted-cycle check covers a restart mid-cycle.
- Firmware alone cannot remove the last 0.3 s of pump dead-heading; a pressure switch or bypass is still recommended (unchanged since 2.5).
- The Spanish wording was written for Colombian usage (usted) but has not been reviewed by the household yet; changes go in `web/es.json` and the `TR`/`TRS` pairs.


## What's new in 2.9

### Safety and reliability
- **No wireless updates during a cycle.** The controller only accepts an update while it is Ready and no tank wash is running. An update mid-wash would switch every valve off and lose the cycle. The Service tab shows whether updates are allowed now.
- **Interrupted cycles are reported.** A marker is written to flash when a cycle starts and at each step, and removed when the cycle ends. If it is still there at power-up, after a power cut, crash or watchdog restart, the controller:
  - shows `INTERRUPTED` / *Check machine* on its screen,
  - sends a high-priority alert,
  - logs the event,
  - shows a notice on the app's Overview until someone presses OK or starts a new cycle.

  The cycle is not resumed automatically, because the machine's state is unknown after a power cut.
- **Watchdog.** If `loop()` freezes for 5 s, the controller restarts with every output off, and the interrupted-cycle check takes over. The watchdog is paused during wireless updates.
- **Real valve and pump states.** The status reports what each SSR is actually set to (`outputs` bits: 1 tap, 2 drain, 4 fill, 8 pump, 16 tank wash), and the Service tab shows them.
- **Event log.** The last 50 events are kept in RAM and shown on the Service tab (`GET /api/events`): start-up and restart reason, steps, "machine filled", pump on/off with the reason, alerts, tank wash, Wi-Fi, updates and restores.
- **Settings travel with the history file.** The downloaded history file now includes the settings (without passwords), and Restore from file restores both.

### Security
- **Password prompt.** While either password is still the default, the app opens a dialog asking for two new passwords. Escape does not close it, and it offers Stop while a cycle is running. The default password can no longer be chosen again.
- **Lockout.** After 5 wrong passwords within a minute, the app refuses all requests for a minute, and the lockout is logged.

### For households
- **Time zone** is chosen from a list; *Other* still accepts a POSIX code.
- **Alert setup.** *Create a private topic* fills in a random, hard-to-guess topic and shows the link to subscribe.
- **Quieter routine alerts.** "Cycle complete", "Tank full" and "Tank wash complete" are low priority, and "Cycle stopped" is minimum priority, so most phones show them silently. Warnings stay high priority.
- **Typical step times.** The controller learns how long steps 1–3 usually take on this machine, and the app shows it, for example *Step running for 12:34. Usually about 25 min*.
- **Optional buzzer** on **D12**: an active buzzer module, switched on in Settings. Two short beeps when a cycle ends; three long beeps on alarms (no flow, pump not pumping, tank wash problems, interrupted cycle).
- **Quick card.** A printable one-page card for the household, with an installer commissioning checklist on page two (separate document).

### Engineering
- **Full-cycle simulation** of the real firmware on a PC (`tests/sim`, 48 checks), plus `tests/run_all.sh` to run every test suite with one command.
- Settings version 6 and statistics version 3 both migrate from the previous versions without losing anything.

### Not in 2.9
- **Spanish screen and app text:** it touches every message and needs a native speaker's review of the wording, so it is a separate step.
- **Splitting `GreyWaterSystem.cpp` into smaller files:** best done after 2.9 has run on the real hardware, so any change in behaviour is easy to trace.

## What's new in 2.8.1: Refresh button

Only the app page changed (`DashboardPage.h`).

- **Refresh button** in the top bar, next to the connection badge. It cancels any update still waiting and asks the controller again at once. It shows *Refreshing…* while working, and screen readers hear "Screen updated" when it finishes.
- **No more frozen screen:** every request to the controller now gives up after 5 s (status) or 8 s (actions). Before, one stalled request on weak Wi-Fi could stop all updates until it finished.
- **Slow updates are visible:** if no update arrives for 6 s, the badge changes to *Updating slowly* (amber), so people know when to press Refresh. After two failed updates it shows *Offline*, as before.
- **Fix:** the connection badge and Refresh sit at the right of the top bar again; an earlier change had moved them next to the title.

## What's new in 2.8: the water history is never lost

**Problem:** after updating the firmware, the Water use tab could come back empty.

**Why:** a normal update (USB/DFU or wireless) writes only the program and leaves the history storage (NVS) alone. The history is lost in two cases:
- **The flash is erased:** Burn Bootloader, recovery mode, uploading with another board or partition scheme selected, or "Erase All Flash".
- **The firmware cannot read the stored record:** it then started empty, and the next save overwrote the old record with zeros.

Firmware 2.8 protects the history in three layers.

**1. On the controller**
- The history is written twice ("stats" and a backup copy "stats_bak"); if one is damaged, the other is used.
- A record the firmware cannot read is kept aside ("stats_keep") instead of being overwritten.
- The history is saved again just before every wireless update.
- The Service tab shows where the history came from: *Loaded from the controller*, *Recovered from the backup copy*, *Upgraded from an older firmware*, *Restored from the app*, or *No history stored*.

**2. On every phone that opens the app**
- The app keeps a copy of the history in the phone's browser and updates it whenever the history changes.
- It never replaces a fuller copy with an emptier one.
- If the controller comes back without history, the Water use tab keeps showing the phone's copy, with a banner and a **Restore to controller** button.

**3. As a file**
- **Download history file** and **Restore from file** on the Water use tab.
- The file survives a full flash erase or a replaced board.
- Restoring over existing history asks for confirmation first.

**Notes**
- The phone copy belongs to one browser and one address. Opening the app as `reusewater.local` and by IP address keeps two separate copies, so use the same address each time.
- Settings and the Wi-Fi network are not part of the history file. After a full flash erase they must be set again.
- **New API:** `GET /api/history` (the full history as JSON) and `POST /api/history/restore` (refused if the controller already has history, unless `force=1`). The status adds `history.source` and `history.hasData`.
- **Tests:** `tests/test_history.cpp` adds 19 storage checks. All 52 flow checks still pass.

## What's new in 2.7

Written for the people using the washing machine. Only display texts and the dashboard page changed; the cycle, valve, sensor and pump logic is the same as in 2.6.

### Controller screen (SSD1306)

The screen uses plain words and two key phrases: **Reusing water** (filling from the tank) and **Saving water** (filling the tank). Every message was measured in the screen's font and fits 128 px without scrolling. The header stays within 16 characters.

| When | Header | Message |
|---|---|---|
| Waiting | `READY` | Press START |
| Start pressed | `STARTING` | Starting... |
| Step 1, machine not taking water yet | `1/4 WASH` | Waiting to fill |
| Step 1, filling from the tank / tap / tank too low | `1/4 WASH` | Reusing water / Using tap water / Tank low: tap |
| Step 1, machine filled and washing | `1/4 WASH` | Washing... |
| Step 2 | `2/4 SOAPY DRAIN` | Not saving |
| Step 3, rinse fill / rinsing | `3/4 RINSE` | Using tap water / Rinsing... |
| Step 4 | `4/4 SAVE 18:42` | Saving water / Tank full: drain |
| Any step, no flow for too long | (unchanged) | No water flow |
| Cycle finished | `DONE` | Reused 26 L (or Cycle complete) |
| Cycle stopped | `STOPPED` | Cycle stopped |
| Manual tank wash | `TANK WASH 9:42` | Cleaning tank |
| Tank wash finished / stopped / problem | `TANK CLEANED` / `WASH STOPPED` | Tank full / Stopped early / Check tank |
| Wi-Fi setup | `ReuseWater-Setup` | Join this Wi-Fi |
| Wi-Fi connected | IP address | Connected |

### Dashboard tabs

| Tab | For | Shows |
|---|---|---|
| Overview | Household | Current status, Start and Stop, storage tank, Wash tank |
| Water use | Household | Last wash, this month (share of reused water), monthly figures, totals by period |
| Service | Installers and technicians | Flow sensors (rate, state, pulses), valves and pump, tank level switches, current step with fill progress and guard, controller details |
| Settings | Everyone | Unchanged |

- **Same wording as the screen:** step names are Wash, Soapy drain, Rinse and Save.
- **Valve and pump states (Service tab):** the tank-wash valve state is reported by the controller. The other states are expected values, worked out from the current step and fill source, because the status API does not report each relay.
- **Layout and accessibility:** the tab bar wraps on small screens. No tab scrolls sideways, even at 320 px with text enlarged 200%. All tabs pass axe-core with 0 violations, and 17 of 17 targeted accessibility checks pass.

## What's new in 2.6

### Fix: the cycle moved from Fill to Drain while the machine was still filling

**Cause:** a regression in 2.4. Fill 1/4 ended once it had seen the machine take water *and* a drain start, in any order. Version 2.3 had discarded any drain seen before the fill; 2.4 dropped that ordering. As a result, either of these made the step end the moment the fill was detected:
- a drain-pump run at the start of the machine's program, which many machines do to empty leftover water;
- a few seconds of interference on the drain wire, for example when the tank pump starts.

Tests S9 and S10 reproduce it: with 2.5, Fill 1/4 ended 4 s and 9 s into the fill.

**Fix: a "machine is filled" guard** (`CycleGates` in `FlowMeter.h`). In Fill 1/4 and Rinse 3/4, a drain only ends the step if it **starts after** the machine has:
1. taken water in this step (flow already running at START counts),
2. taken at least the **Minimum fill** (new setting, default 3 L, range 0.5–50 L), and
3. stopped taking water (its inlet has closed).

Top-ups during the wash are fine. If the inlet never closes, the step waits and the step watchdog alerts.

**Dashboard:** during Fill and Rinse, the timing line shows the litres taken and whether the guard is set, for example *Filling: 2.1 L taken of at least 3 L*, then *Filled (11.6 L taken); waiting for the drain*. The status API adds a `fill` object.

**Settings migration:** settings from 2.4 and 2.5 migrate with the new default added, so there is no reset this time. `SETTINGS_VERSION` is 5.

The tests now total 52 checks, including S9–S13 for this fix. All earlier drain and pump tests still pass.

## What's new in 2.5

### Tank pump protection

In 2.4 the tank pump was switched by the slow flow meter (2 s average, 2 s stop delay). When the washing machine closed its inlet valve, the pump kept pushing against it for about 3.7 s on every draw, which wears a pump out.

`PumpGuard` (in `FlowMeter.h`) now switches the pump off on its own:

- **Fast cut-off:** on every `loop()` pass while the pump runs, if no tap-sensor pulse arrives for `PUMP_NO_FLOW_MS` (300 ms), the pump stops and the tap takes over at once. At fill rates of 5–10 L/min, pulses arrive every 10–20 ms.
- **Start-up grace:** the pump has `PUMP_START_GRACE_MS` (1 s) after starting for water to flow, so the dip while the tap valve closes and the pump spins up does not trip it.
- **No restart into a closed inlet:** after a cut-off, the pump stays off until the machine starts a new draw (a new confirmed flow on the tap sensor). If the machine pauses for less than about 2 s between draws, the next draw is supplied from the tap.
- **Dry-start alert:** a start with no water at all (air lock, blocked intake, dry pump) also stops within about 1.3 s. After `PUMP_DRY_ALERT` (3) dry starts in one cycle, a high-priority phone alert is sent.

Measured in the PC tests (`tests/`, P1–P5), the pump runs against a closed inlet for 0.29 s, down from 3.75 s in 2.4. Over five bursts of filling, the total drops from 19 s to 1.45 s. The time to switch *to* tank water is unchanged (about 4 s into each draw).

Firmware alone cannot remove the last 0.3 s. For full protection, use a pump with a built-in pressure switch, or fit a pressure switch or a small bypass back to the tank.

## What's new in 2.4

### Fix: drain sensor did not advance the cycle

**Symptom:** the cycle stayed in a step, usually Fill2 (3/4 RINSE), and only moved on after the drain flow sensor was unplugged and plugged back in.

**Causes found:**
1. **The flow-start detector latched.**
   - `detectRisingEdge()` kept a static "detected" flag that was cleared only when it was itself called with an exact zero reading, and no step re-armed it.
   - If at least one pulse arrived in every 250 ms window between the two drains, it stayed latched and Fill2 never saw the rinse drain start. That takes a steady pulse train of about 4 pulses/s or more: a trickle of about 0.7 L/min, or electrical pickup on the signal wire.
   - Unplugging the sensor forced a zero reading, which re-armed it; the step then ended 4 s after plugging back in.
   - `tests/old_detector_repro.cpp` and `tests/old_cycle_compare.cpp` reproduce this with the unchanged 2.3 code.
2. **There was no flow threshold.** One pulse in a 250 ms window counted as flow, and one empty window reset the 4 s timer. Noise looked like flow, while slow pulsed flow was never detected.
3. **The signal level could be marginal (wiring).**
   - The FlowSensor library's `begin(fn, pullup)` parameter is inverted, so the internal pull-up was on.
   - Together with the 10 kΩ/20 kΩ divider recommended in runbook 1.1–2.3, a sensor with an internal pull-up gave a high level of about 2.5–2.65 V, against the ESP32-S3's 2.48 V minimum.
   - Any contact resistance in a connector could stop the pulses, which may be why reseating the plug helped.

**Changes:**
- **`FlowMeter.h`** replaces the old detector and the FlowSensor library. Its rate is a 2 s average.
- **Threshold with time-based debounce:** water counts as flowing above the *Flow threshold* (default 1 L/min, adjustable in Settings) for the flow detection time, and as stopped below it for 2 s. Gaps up to 1 s are tolerated. Steady noise below the threshold can no longer block a step.
- **Continuous updates:** the meters update on every `loop()` pass. Each step re-arms detection when it begins (`CycleGates`), so nothing stays latched.
- **Glitch filter:** the interrupts ignore pulses closer than 1 ms apart, such as relay switching spikes.
- **Library removed:** the FlowSensor library is no longer needed. Its flow-rate calculation divided by whole seconds and returned infinity or NaN.
- **Sensor check:** the dashboard's Controller panel shows each sensor's pulse count and whether it reads flow.
- **Tests:** `tests/` holds the PC test checks (46 from 2.5), including the reported fault, plus the old code run through the same scenarios (see `tests/README.md`).
- **Limit:** interference at mains frequency (50 or 60 Hz) on the signal wire looks exactly like about 10 L/min of flow, so no firmware can filter it out. If the dashboard's sensor check counts pulses while no water flows, fix the wiring.

**Wiring change, recommended:** replace the flow sensors' 10 kΩ/20 kΩ divider.
- Fit a small Schottky diode (for example BAT54 or 1N5819) from the sensor signal to the pin, cathode toward the sensor.
- Add a 10 kΩ pull-up from the pin to 3V3.
- This works whether the sensor output is open-collector, has its own pull-up, or is push-pull. The pin sees about 0.3 V low and 3.3 V high.

### Accessibility fixes (from the audit)

- **Contrast:** text contrast is at least 4.5:1, and control outlines and progress segments at least 3:1, in both themes.
- **Step tracker:** completed steps show a check mark and the hidden text "completed". The current step has `aria-current`.
- **High contrast mode:** the selected tab, unavailable buttons and progress are all distinguishable.
- **Announcements:** the screen reader hears step changes, the end of a cycle and warnings once, from dedicated announcement regions. The connection badge no longer repeats every 2 s.
- **Settings errors:** they appear under the field, the field is marked invalid, and the error message stays until dismissed.
- **Unavailable buttons:** Start, Stop and Wash tank stay focusable with their reason read out. Focus is no longer lost when a button becomes unavailable.
- **Headings:** an `h1` and a status heading. The tabs work with the arrow keys, Home and End.
- **Field names:** each field's name is its label only; the hint and the unit ("in seconds") are read as its description.
- **Text size:** sizes are in `rem`, so the browser's text-size setting applies. The tank drawing is hidden from screen readers, because the facts list carries the level.
- **Controller screen:** short static messages that fit without scrolling, such as *Tap water*, *Draining* and *Check sensors*. The dashboard keeps the full sentences.

Axe-core reports 0 violations in every state, and 17 of 17 targeted checks pass.

`SETTINGS_VERSION` is now 4 (adds the flow threshold), so settings reset to the defaults once. Statistics are kept.

## What's new in 2.3

The dashboard now shows what the system achieves, not just what it is doing.

- **"This month" panel:**
  - It leads with the share of the washing machine's fill water that came from the tank, for example *62%*, with a tank/tap split bar.
  - A comparison with the previous month, for example *Better than September*.
  - The litres translated into washes, using this controller's own average fill: *About 4 washes worth of fill water came from the tank*.
  - The all-time total.
  - The litre table is still there, under **Details**.
- **Last wash summary:**
  - The Ready screen shows when the last wash ended, how long it took, and the litres reused and saved.
  - Stopped washes are labelled as stopped.
  - The summary is stored in flash, so it survives a power cut.
- **Readiness instead of an empty screen:** the Ready state says what the next wash will get, using the tank level and water age:
  - *The tank has water ready for the next wash, stored for 5 h.*
  - *The tank is empty, so the next wash will use tap water.*
  - A warning when the water is past its age limit.
  - The empty progress bar is hidden until a cycle starts.
- **Empty states:** a new install or a new month shows one clear sentence instead of rows of zeros.
- **Disabled buttons:** they now turn neutral grey instead of a faded red or green.
- **Statistics upgrade:** `STATS_VERSION` 2 adds the previous month and the last wash. Statistics stored by earlier versions are migrated, so the all-time totals are kept.
- **Fix:** washes recorded before the clock was set (no Wi-Fi yet) used to be dropped from "this month" when the clock arrived. They are now kept.
- `/api/status` adds `prevMonth` and `last` objects.

## What's new in 2.2

- **Manual tank wash** from the dashboard: the **Wash tank** button in the Storage tank panel opens the tank-wash valve (D0) while the system is Ready.
- **Closes when the tank is full:** the valve closes the moment the high-level switch (D8) reports water. This is checked on every `loop()` pass, not every 250 ms.
  - If the high-level switch is wet but the low-level switch (D9) is dry, the switches disagree. The valve still closes, and the OLED and an alert ask you to check the floats. Closing early is safe; overflowing is not.
  - The button is unavailable when the tank is already full, or while a cycle runs.
- **Other ways the wash ends:**
  - **Stop washing** on the dashboard, or the STOP button on the controller.
  - Starting a cycle.
  - A safety time limit (default 10 min, setting *Manual wash time limit*, 1–30 min), in case a level switch has failed. Reaching the limit sends a high-priority alert.
- **Screen and phone feedback:** the OLED shows `TANK WASH m:ss` with the time left before the limit. When the wash ends it shows why (*Tank full: valve closed*, *Tank wash stopped*, or *Time limit: check level switch*). The dashboard shows a matching message, and the full and time-limit endings also send a phone alert.
- `SETTINGS_VERSION` is now 3, so settings saved by 2.1 reset to the defaults once.

## What's new in 2.1

- **Redesigned dashboard:** app bar with connection badge, Overview and Settings tabs, status panel with step tracker, tank panel, four key figures (tap flow, drain flow, water reused and saved this month), savings table and controller details. Light and dark mode.
- **Start and Stop buttons** are always on the dashboard. Start is enabled only when the system is Ready and asks for confirmation; Stop is enabled only while a cycle runs and acts at once. Buttons show *Starting…* / *Stopping…* until the controller confirms.
- **Start from the dashboard is on by default.** It can still be turned off in Settings. `SETTINGS_VERSION` is now 2, so settings saved by 2.0 are reset to the defaults once.
- **Tank-wash valve moved from D1 to D0** (`OUT_WASH_EV D0`). D0 is the RX0 pin, which the board does not drive at power-up, so the valve no longer clicks at boot.

Firmware 2.0 adds Wi-Fi features on the Arduino Nano ESP32. The wash cycle works exactly as before without Wi-Fi; every network feature runs in the background and never delays valve switching.

## What's new in 2.0

| Feature | What it does |
|---|---|
| Dashboard | Web page at `http://reusewater.local`: current step, countdown, tank level, tank-wash valve, live flow rates, water statistics, Start and Stop buttons |
| Phone alerts | Push messages through [ntfy](https://ntfy.sh): cycle complete, cycle stopped, tank full, step stuck (no flow), tank water too old |
| Step watchdog | Steps 1–3 that run longer than the alert time send one alert and show *No flow seen: check sensors* on the OLED. The step keeps waiting, so a slow machine is never cut short |
| Water statistics | Litres of tank water reused, rinse water saved to the tank and tap water used: this cycle, this month, all time. Stored in flash |
| Tank water age | Time since the tank went from empty to holding water. Alert when it passes the limit (default 48 h) |
| Wireless updates | Upload new firmware over Wi-Fi (ArduinoOTA). All valves switch off during the update |
| Wi-Fi setup page | WiFiManager: choose the home network from a phone, no password in the code |
| Settings page | Wash timing, final drain length, flow detection time, alert limits, alert topic, time zone and passwords, saved in flash |
| Clock | NTP time for monthly totals and water age |

### Bug fixed: flow rate

`FlowSensor::read()` divides by whole seconds (`(millis() - _timebefore) / 1000`). At the sketch's 250 ms reads that is 0, so the library's flow rate was infinity during flow and NaN without flow. Flow detection only worked because `inf > 0` is true. The firmware now computes the rate from the library's volume counter, which is correct, so the dashboard shows real L/min values and the volumes are accurate.

## Files

| File | Contents |
|---|---|
| `FSM_WashingMachineSystem.ino` | `setup()`, `loop()`, state machine wiring |
| `GreyWaterSystem.h/.cpp` | Wash cycle: buttons, steps, manual tank wash, statistics, tank age, status snapshot |
| `GreyWaterOutputs.h/.cpp` | The five SSR outputs, float switches, safety interlocks, pump cut-off (3.0) |
| `GreyWaterTask.cpp` | Safety task (every 20 ms) and the lock it shares with `loop()` (3.0, FreeRTOS) |
| `GreyWaterSensors.h/.cpp` | Flow-sensor interrupts and meters (3.0) |
| `GreyWaterDisplay.cpp` | OLED drawing (3.0) |
| `GreyWaterDiagnostics.h/.cpp` | Event log, loop timing, buzzer, restart reason (3.0) |
| `GreyWaterSettings.h/.cpp` | Settings and statistics stored in flash (Preferences) |
| `GreyWaterNet.h/.cpp` | Wi-Fi, setup page, dashboard API, wireless updates, NTP, alerts |
| `FlowMeter.h`, `TankLevel.h` | Hardware-free logic: flow detection, step gates, pump guard, float debouncing (unit tested) |
| `web/dashboard.html` | The dashboard page (source) |
| `DashboardPage.h` | The page, gzip-compressed, generated by `tools/build_dashboard.py` |
| `docs/` | Architecture, protocol, installation, testing |
| `tests/` | PC tests and simulation (`sh tests/run_all.sh`) |

## Libraries

Install with the Arduino IDE Library Manager:

- **WiFiManager** by tzapu (new in 2.0)
- Adafruit GFX Library, Adafruit SSD1306, AgileStateMachine (FlowSensor is no longer needed from 2.4)

`WiFi`, `WebServer`, `ESPmDNS`, `ArduinoOTA`, `HTTPClient`, `WiFiClientSecure` and `Preferences` come with the ESP32 board package.

Board: **Arduino Nano ESP32**, pin numbering **By Arduino pin (default)**. Verified build of 3.0 with the Espressif ESP32 core 3.0.7: 1.30 MB flash (41% of the 3 MB app slot, so wireless updates have room) and 22% RAM. Also builds with core 2.0.17.

## First-time setup

1. Upload the firmware over USB once.
2. On first boot the OLED shows `ReuseWater-Setup` and `Key gw-xxxxxx`. On a phone, join the Wi-Fi network **ReuseWater-Setup** with that key (it is unique to the board and also printed on the USB serial monitor at 9600 baud). If the board has no working screen, the key is `greywater`.
3. A setup page opens (if not, browse to `192.168.4.1`). Choose the home network and enter its password.
4. The controller connects and the OLED shows its IP address for 6 seconds.
5. Open `http://reusewater.local` (or the IP address). Sign in as `admin` / `greywater`.
6. Open **Settings**: set new dashboard and update passwords, and an alert topic.
7. Install the **ntfy** app on the phone, subscribe to the same topic, and press **Send a test alert**.

The setup page closes after 3 minutes; the system then runs without Wi-Fi. To open it again (new router, new password), hold **STOP for 5 seconds** while the screen shows `READY`.

## Wireless firmware updates

In the Arduino IDE, choose **Tools > Port > reusewater at <IP>** (network port), then Upload. Enter the update password when asked. Updates are accepted only while the controller is Ready and no tank wash runs (since 2.9). The controller switches all valves and the pump off, shows `UPDATING`, and restarts when finished.

## Dashboard API

All pages need the dashboard password (HTTP Basic, user `admin`). Actions (`POST`) also need the header `X-GW: 1`, which blocks other websites from pressing buttons with the remembered password.

| Method | Path | Purpose |
|---|---|---|
| GET | `/` | Dashboard page |
| GET | `/api/status` | Live status and statistics (JSON) |
| GET | `/api/settings` | Current settings (JSON, passwords not included) |
| POST | `/api/settings` | Save settings (form fields, same names as the GET response, plus `webPassword`, `otaPassword`) |
| POST | `/api/stop` | Stop the cycle (409 when no cycle runs, from 3.0) |
| POST | `/api/start` | Start a cycle (when enabled in Settings, on by default, and the system is Idle) |
| POST | `/api/wash` | Start a manual tank wash (only while Idle and the tank is not full) |
| POST | `/api/wash/stop` | Stop the manual tank wash |
| POST | `/api/test-alert` | Send a test alert |
| POST | `/api/restart` | Restart the controller (only in Idle) |
| GET | `/api/history` | Water history as JSON, with settings (no passwords) |
| POST | `/api/history/restore` | Restore history (409 if history exists, unless `force=1`) |
| GET | `/api/events` | The last 50 events |
| POST | `/api/interrupted/dismiss` | Hide the interrupted-cycle notice |

Full message formats, status fields and error codes: `docs/PROTOCOL.md`.

## Settings (web page)

| Setting | Default | Range |
|---|---|---|
| Wash every | 10 s | 1–600 s |
| Keep the valve open | 2 s | 0.25 s up to the interval minus 0.25 s |
| Manual wash time limit | 10 min | 1–30 min |
| Final drain length | 20 min | 1–120 min |
| Flow detection time | 4 s | 1–30 s |
| Start from the dashboard | On | On / off |
| Warn when a step takes longer than | 90 min | 10–600 min |
| Warn when tank water is older than | 48 h | 1–240 h |
| Alert server | `https://ntfy.sh` | Any ntfy server |
| Alert topic | Empty (alerts off) | — |
| Time zone | `<-05>5` (UTC−5) | POSIX TZ string |
| Dashboard / update passwords | `greywater` | 8–32 characters |

The `#define`s in `GreyWaterSystem.h` are the factory defaults for first boot. Changing them later has no effect until the stored settings are reset (bump `SETTINGS_VERSION` in `GreyWaterSettings.h`).

## Behavior notes

- **Offline first:** Wi-Fi connects in the background. Without Wi-Fi the cycle, OLED and buttons work as before; alerts raised while offline are sent if Wi-Fi returns within 60 seconds, otherwise dropped.
- **Remote Stop** acts exactly like the STOP button. **Remote Start** asks for confirmation every time, because nobody may be at the machine; turn it off in Settings if that is a risk.
- **Water accounting:** flow through the tap sensor counts as *tank water reused* while the pump fills from the tank, and as *tap water* during tap fills. Flow through the drain sensor counts as *saved to the tank* while step 4 saves water. This assumes the tap sensor sits on the machine inlet after the tank and tap lines join, which is how the fill logic already treats it.
- **Tank water age** starts when the level rises above the low switch and resets when it falls below it again, so it reports the age of the oldest water. It needs three equal level readings 10 s apart, so sloshing does not reset it.
- **Statistics** are written to flash at the end of each cycle and when the tank age starts or resets, a few writes per day.
- **Alerts use HTTPS without certificate checking.** They contain no secrets; anyone who knows the topic name can read them, so use a long, random topic.
- **Security:** keep the dashboard on the home network. Do not forward router ports to it. Change both default passwords on first use.
- **Float switches (3.0):** the tank is full whenever the high float is wet; the pump is used only when the low float is wet. These rules are enforced every 20 ms by the safety task, in every state.
- **Pin D0** (tank-wash valve) is also the RX0 serial pin. `Serial` uses USB, so do not use `Serial0`. A 10 kΩ resistor from D0 to GND keeps the SSR firmly off while the board starts.

---

## Earlier changes (1.x)

This update improves the OLED display and its messages, and adds periodic washing of the grey-water tank while the washing machine fills with **tank water**, using a dedicated solenoid valve (`OUT_WASH_EV`). While the tank-water fill is active, the wash valve opens every 10 seconds.

## Summary

| File | Change |
|---|---|
| `GreyWaterSystem.h` | `OUT_WASH_EV` set to `D1`; tank-wash constants; display constants; `FreeSans9pt7b` font; new display function prototypes (replace `testscrolltext()`) |
| `GreyWaterSystem.cpp` | Periodic tank-wash logic in `fillWashingMachineWithTankWater()`; new OLED display module; clearer messages for every state; Drain2 countdown |
| `FSM_WashingMachineSystem.ino` | `OUT_WASH_EV` added to `allSSROff()`; `displayUpdate()` called every `loop()`; boot message uses the new display |

## How the tank wash works

`setRelayState()` takes five outputs in this order:

```cpp
setRelayState(tap, drain, fill, motor, wash);
```

When `fillWashingMachineWithTankWater()` runs, the wash output comes from `tankWashOutput()`:

```cpp
void fillWashingMachineWithTankWater() {
  DEBUG_PRINT("Filling with tank water + washing tank.");
  setRelayState(ssrOFF, ssrON, ssrOFF, ssrON, tankWashOutput());
}
```

1. The first call in a state starts the wash timer, and the valve opens immediately.
2. The valve stays open for `TANK_WASH_OPEN_TIME` ms, then closes.
3. Every `TANK_WASH_PERIOD` ms (10 s) the valve opens again, for as long as the tank-water fill continues.
4. `resetTankWash()` restarts the timer on every state entry (and in Idle).

Timeline with the default settings:

```
time (s):  0    2         10   12        20   22
valve:     OPEN-closed----OPEN-closed----OPEN-closed ...
```

### When the tank is washed

The tank-water fill only happens in **Fill1 (1st Fill)**, when the tank is not low and tap demand has been detected. That is the only time the wash valve opens.

When Fill1 switches back to tap water, the wash valve closes. If the fill switches back to tank water in the same state, the 10-second cycle continues from where it started rather than restarting.

The wash valve is **closed** during idle, run, tap-water filling, draining soapy water, saving water to the tank, and when the tank is full.

## Configuration

In `GreyWaterSystem.h`:

```cpp
#define OUT_WASH_EV D1             // Wash tank solenoid valve pin
#define TANK_WASH_PERIOD 10000     // ms between valve openings (opens every 10 s)
#define TANK_WASH_OPEN_TIME 2000   // ms the valve stays open on each opening
```

- `TANK_WASH_OPEN_TIME` must be smaller than `TANK_WASH_PERIOD`. If it is equal or larger, the valve stays open for the whole tank-water fill.
- The state logic runs every `OPERATION_DELAY` (250 ms), so the open time is effectively rounded to 250 ms steps. Use multiples of 250 ms for predictable timing.

## Detailed changes

### `GreyWaterSystem.h`

- `OUT_WASH_EV` moved to `D1`. On the Arduino Nano ESP32, D1 is also the TX0 pin of UART0. `Serial` in this sketch uses the USB port, so D1 is free, but do not use `Serial0` in the same sketch. During power-up the board may briefly pulse TX0, so the tank-wash valve can click for a moment at boot.
- Added `TANK_WASH_PERIOD` and `TANK_WASH_OPEN_TIME`.
- Added the prototype `void resetTankWash();`.

### `GreyWaterSystem.cpp`

- **New tank-wash block:** `tankWashStarted`, `tankWashStartTime`, `resetTankWash()` and `tankWashOutput()`.
- **`fillWashingMachineWithTankWater()`:** the wash output is `tankWashOutput()`, which opens the valve every `TANK_WASH_PERIOD` ms.
- **`fillWashingMachineWithTapWater()`:** the wash output is `ssrOFF`.
- **`checkAndFillWaterTank()`:** the "Tank Full - Standing By" branch calls `noSavingWater()`. It sets the same outputs as a tap fill, with the wash valve closed.
- **`handleIdleStateEntry()` and `handleFillDrainEntry()`:** call `resetTankWash()`.

### `FSM_WashingMachineSystem.ino`

- `allSSROff()` includes `OUT_WASH_EV`, so the wash valve is forced off at boot like the other outputs.
- `pinMode(OUT_WASH_EV, OUTPUT);` in `setup()`.

## Relay table

| Function | Tap | Drain | Fill | Motor | Wash |
|---|---|---|---|---|---|
| `fillWashingMachineWithTankWater()` | OFF | ON | OFF | ON | Opens every 10 s for `TANK_WASH_OPEN_TIME` |
| `fillWashingMachineWithTapWater()` | ON | ON | OFF | OFF | OFF |
| `drainSoapyWater()` | ON | ON | OFF | OFF | OFF |
| `noSavingWater()` | ON | ON | OFF | OFF | OFF |
| Save water to tank (`checkAndFillWaterTank()`, low/medium) | ON | OFF | ON | OFF | OFF |
| `allSSROff()` (boot) | OFF | OFF | OFF | OFF | OFF |

## OLED display

### Layout

The 128x32 screen is split into a header and a message area:

```
+--------------------------------+
|1/4 FILL               [##] [W] |   header: step + tank level + wash valve
|--------------------------------|
|        Using tank water        |   message: centred, or scrolled if too long
+--------------------------------+
```

- **Header (small built-in font):** the current step and the cycle progress (for example `1/4 FILL`). In Drain2 it also shows the time left before the cycle finishes, for example `4/4 DRAIN 18:42`.
- **Tank icon:** empty when the tank is low, half filled at medium, full at high. It reads the level sensors directly, so it is always live.
- **Wash icon `W`:** outlined when the tank-wash valve is closed, inverted (white box) when it is open. During the tank-water fill you will see it light up every 10 seconds.
- **Message (Helvetica Bold pixel font, `FontHelvBold14.h` / `FontHelvBold12.h`):** a bitmap font drawn for screens, so every stroke sits on whole pixels and stays sharp. Each message uses 14 px when it fits the 128 px width, otherwise 12 px; every screen text fits in 12 px (`tools/check_screen_text.py`). Messages are centred and stay still; a longer one would pause for 1.5 s, then scroll smoothly and loop with a gap.

### Messages

| State | Header | Message |
|---|---|---|
| Boot (about 4 s) | — | Start-up animation, full screen: a Border Collie chases a butterfly (`BootAnimation.h`). START or a tank wash ends it; notices that arrive meanwhile are shown after "System ready" |
| Boot (2 s) | `REUSE WATER` | System ready |
| Idle | `READY` | Press START |
| Run | `STARTING` | Starting cycle... |
| Fill1, tank low | `1/4 FILL` | Tank low: using tap water |
| Fill1, tap water | `1/4 FILL` | Using tap water |
| Fill1, tank water | `1/4 FILL` | Using tank water |
| Drain1 | `2/4 DRAIN` | Draining soapy water |
| Fill2 | `3/4 RINSE` | Rinsing with tap water |
| Drain2, saving | `4/4 DRAIN mm:ss` | Saving water to tank |
| Drain2, tank full | `4/4 DRAIN mm:ss` | Tank full: not saving |
| After Stop (4 s) | `STOPPED` | Cycle stopped |
| After a full cycle (4 s) | `DONE` | Cycle complete |

The trailing spaces that were used as padding in the old messages have been removed; spacing is now handled by the layout.

### How it works

State logic only sets the text; one function does all the drawing.

```cpp
displaySetStatus("2/4 DRAIN", "Draining soapy water");          // normal status
displayShowNotice("STOPPED", "Cycle stopped", NOTICE_DURATION);  // temporary message
displayClearNotice();                                            // remove it early
displayUpdate();                                                 // called every loop()
```

- `displayUpdate()` runs on every `loop()` pass and draws at most one frame every `DISPLAY_FRAME_MS`. Before, the scroll could only move when a state's 250 ms timer fired, which is why it jumped 30 px at a time.
- A frame is only sent over I2C when something visible changed (text, tank level, wash valve) or while a long message is scrolling. Static screens cost no bus time.
- A notice overrides the status for its duration, so "Cycle stopped" and "Cycle complete" are no longer overwritten by "Press START" a quarter of a second later. Pressing Start clears the notice immediately.
- Text wrapping is turned off, so long messages no longer spill onto a second line while scrolling.
- Messages are stored in fixed `char` buffers instead of `String`, which avoids heap fragmentation on a system that runs for long periods.
- The display no longer reads the buttons. `loop()` already calls `updateInputs()`, so the extra call inside the old `testscrolltext()` has been removed.

### Display settings

In `GreyWaterSystem.h`:

| Constant | Default | Meaning |
|---|---|---|
| `DISPLAY_FRAME_MS` | 40 | ms between frames while scrolling (25 fps) |
| `SCROLL_SPEED_PX` | 2 | px moved per frame (50 px/s) |
| `SCROLL_PAUSE_MS` | 1500 | ms a long message rests before scrolling |
| `SCROLL_GAP_PX` | 40 | px of blank space before the message repeats |
| `NOTICE_DURATION` | 4000 | ms "Cycle stopped" / "Cycle complete" stays on screen |

For faster scrolling, raise `SCROLL_SPEED_PX` to 3. Lowering `DISPLAY_FRAME_MS` below about 20 ms makes the I2C transfers take a large share of `loop()` time.

### Display fixes

- **"Cycle Complete!" at power-up:** at boot, `onEnter()` runs the Idle entry with Stop not pressed, which used to show "Cycle Complete!". The notice now only appears after a cycle has actually been started.
- **`testscrolltext()` replaced:** the function and the `SCROLL_STEP_DELAY` / `SCROLL_STEP_PX` constants were removed. If other code calls `testscrolltext()`, replace it with `displaySetStatus()`.
- **`delay(300)` in `setup()` removed:** the boot message is now a 2-second notice and does not block.

## Testing checklist

1. Power on: confirm the wash valve is closed.
2. Start a cycle with the tank low: Fill1 uses tap water and the wash valve must stay closed.
3. Start a cycle with water in the tank: once Fill1 switches to tank water, the wash valve should open immediately, close after 2 s, and reopen every 10 s.
4. Fill1 switching from tank water to tap water: the wash valve closes within 250 ms.
5. Fill2 (rinse) and Drain2: the wash valve must stay closed.
6. Press Stop during any state: the system returns to Idle and the wash valve closes within 250 ms.
7. Power on: the screen shows the Border Collie chasing a butterfly (about 4 s), then `REUSE WATER / System ready` for 2 s, then `READY / Press START`, with no "Cycle complete" message.
8. During each step, the header shows the right step (`1/4` to `4/4`) and long messages scroll smoothly without jumping.
9. During the tank-water fill, the `W` icon turns white every 10 s and the tank icon follows the level sensors.
10. In Drain2, the countdown decreases every second.
11. Press Stop: `STOPPED / Cycle stopped` stays for 4 s, then `READY / Press START`.
