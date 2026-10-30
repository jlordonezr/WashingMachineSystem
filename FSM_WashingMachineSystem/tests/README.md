# Tests

Firmware 3.0 adds: `test_level.cpp` (float switches, 19 checks), simulation scenarios F7–F11 (failed float, bobbing float, no screen, `loop()` stalled while pumping and while saving), and `app/test_dashboard.js` (the dashboard in headless Chromium, 21 checks including axe-core). `run_all.sh` now exits with an error if any suite fails. The full strategy, including the hardware checks, is in `docs/TESTING.md`.


Run everything with one command from the sketch folder: `sh tests/run_all.sh`. Set `ARDUINO_CLI` to also compile the firmware.

## Full-cycle simulation (`tests/sim`)

`sh tests/sim/run_sim.sh` builds the **real** firmware (the sketch, `GreyWaterSystem.cpp`, `GreyWaterSettings.cpp`, `FlowMeter.h` and the AgileStateMachine library) for the PC. It runs it against a simulated washing machine, tank, flow sensors, level switches and buttons. It needs the AgileStateMachine library; set `AGILE` to its `src` folder if it is not in `~/Arduino/libraries`.

| Scenario | Checks |
|---|---|
| F1 full cycle reusing tank water | Each step starts at the right moment; pump starts when the machine draws; pump and tap valve never on together; pump stops within 0.35 s of the inlet closing; rinse water saved; every screen message shown; statistics and typical step times recorded; low-priority completion alert; event log; cycle marker cleared |
| F2 drain pump at program start | Wash step does not end early |
| F3 STOP during the wash | Ready again, outputs safe, minimum-priority alert, marker cleared |
| F4 power cut during the rinse | Marker in flash during the cycle; after power-up: interruption detected, alert, event, INTERRUPTED on screen, outputs safe, marker cleared, notice cleared by the next Start |
| F5 normal power-up | No false interruption alert |
| F6 watchdog restart while Ready | Logged; no interruption reported |
| F7 low float failed dry, tank full (3.0) | Fault alert; pump never runs; fill and wash valves never open into the full tank; cycle completes |
| F8 high float bobbing at the mark (3.0) | Fill valve switches at most twice; tank stops below 39 L |
| F9 screen missing (3.0) | Runs without it, alert, cycle completes |
| F10 loop() blocked 3 s while pumping (3.0) | Safety task still stops the pump within 0.35 s (2.9.3: 3.2 s) |
| F11 loop() blocked 3 s while saving (3.0) | Safety task closes the fill valve; no overfill (2.9.3 overfills) |

The simulation found a real bug before release: Ready-entry at power-up cleared the cycle marker before the interruption check could read it.

# Flow detection tests

These tests run on a PC; no board is needed. They test `FlowMeter.h`, which is the same file the firmware uses.

```
cd tests
g++ -std=c++17 -O1 -Wall test_flow.cpp -o test_flow && ./test_flow
g++ -std=c++17 -O1 old_detector_repro.cpp -o old_detector && ./old_detector
g++ -std=c++17 -O1 old_cycle_compare.cpp -o old_cycle && ./old_cycle

# water history storage (uses the stub headers in stubs/)
cp ../GreyWaterSettings.cpp ../GreyWaterSettings.h stubs/
g++ -std=c++17 -O1 -Wall -Istubs test_history.cpp stubs/GreyWaterSettings.cpp -o test_history && ./test_history
```

`test_flow` exits with code 0 when all checks pass. `test_results.txt` holds the output from the last run.

| File | What it shows |
|---|---|
| `old_detector_repro.cpp` | The firmware 2.3 drain detector, copied unchanged. It reproduces the fault: stuck while a pulse arrives in every 250 ms window, advancing only after the sensor is unplugged and plugged back in |
| `old_cycle_compare.cpp` | The firmware 2.3 step logic run through the same cycle scenarios as the new tests (-1 = stuck) |
| `test_history.cpp` | Water history storage in `GreyWaterSettings.cpp` against a simulated flash store: survives restarts, recovers from the backup copy, keeps unreadable records, upgrades old records, restores from the app, refuses invalid backups (20 checks, including the upgrade from the 2.3–2.8 format) |
| `test_level.cpp` | `TankLevel.h`: debouncing towards the safe reading, bobbing float, failed low float, fault timing, millis() wrap (3.0) |
| `app/test_dashboard.js` | The real `web/dashboard.html` against a mock controller: confirm-before-success, timeouts, float fault, alert results, Service tab, accessibility (3.0) |
| `test_flow.cpp` | 12 unit tests (T01–T12), 9 cycle scenarios (S1–S8), 5 "machine is filled" guard tests (S9–S13) and 5 pump-protection tests (P1–P5), 52 checks in total |

## Test cases

| ID | Scenario | Expected |
|---|---|---|
| T01 | No pulses for 60 s | Never flowing, rate 0 |
| T02 | Steady 18 L/min drain | Flowing after 4 s, one start, rate within 5% |
| T03 | Noise at 1 pulse/s | Never counts as flow |
| T04 | Drain followed by a 0.4 L/min trickle | Returns to stopped |
| T05 | 2.9 L/min with 300 ms gaps every second | Flowing (gaps tolerated) |
| T06 | Flow, 5 s stop, flow | Two starts |
| T07 | 1.5 s interruption during a drain | Still one start |
| T08 | 2 s splash | Ignored |
| T09 | 10 L/min for 60 s | 10 L measured within 1% |
| T10 | Pulse counter and millis() wrap past 2^32 | Detection and rate unaffected |
| T11 | 20 spikes 50 µs apart, then real pulses | Spikes rejected, real pulses kept |
| T12 | Steady 5 noise pulses/s after a drain | Returns to stopped |
| S1 | Typical cycle | Each step ends within 6 s of the real event |
| S2 | Trickle between drains (the reported fault) | Fill2 ends on the rinse drain |
| S3 | 2 noise pulses/s all cycle | No early or blocked steps |
| S4 | Slow gravity drain with gaps | Detected |
| S5 | Machine already filling when START is pressed | Counts as the Fill1 water intake |
| S2b | Steady 5 pulses/s between drains (what stuck the old code) | Fill2 ends on the rinse drain |
| S6 | Soapy drain still running when Fill2 starts | Waits for the new (rinse) drain, so soapy water is not saved |
| S7 | Real 1.5 L/min trickle between drains | Waits at the default 1 L/min threshold; advances with the threshold at 3 L/min |
| S8 | 60 Hz mains pickup on the drain wire | Reads about 10 L/min with no water, visible in the sensor check; needs the wiring fix |
| S9 | Drain pump runs at program start, then the machine fills | Fill1 waits for the wash drain (firmware 2.5 ended it 4 s into the fill) |
| S10 | 6 s of interference on the drain wire while filling | Ignored (2.5 ended Fill1 9 s into the fill) |
| S11 | 1 L splash, a drain, then the real fill | The splash is not taken as the fill |
| S12 | Machine tops up twice during the wash | Fill1 still ends on the wash drain |
| S13 | Machine inlet never closes | Fill1 waits; the step watchdog alerts |
| P1 | Machine closes its inlet while the tank pump runs | Pump stops within 0.35 s (measured 0.29 s; firmware 2.4 ran on for 3.75 s) |
| P2 | Pump needs 0.6 s to start delivering | Not mistaken for the inlet closing |
| P3 | Pump delivers no water (dry, air lock) | Stops within 1.3 s, tap takes over, no retry on the same draw, counted as a dry start |
| P4 | Machine draws in five 8 s bursts | One pump start per burst; at most 0.35 s run-on each (total 1.45 s; firmware 2.4: 19 s) |
| P5 | Only 1 s pause between two draws | Pump stays off for the second draw; tap supplies it |
