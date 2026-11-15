# Reuse Water System – rules for Claude

Firmware for one **Arduino Nano ESP32** (`esp32:esp32:nano_nora`) that runs the wash-cycle state machine, sensors, 5 SSR outputs, a 128×32 OLED and a web dashboard served from the board. Sketch folder: `FSM_WashingMachineSystem/`. Read `docs/ARCHITECTURE.md` and `docs/PROTOCOL.md` before changing behaviour.

## Always
- After any change to firmware, dashboard, translations or tools, run `sh FSM_WashingMachineSystem/tests/run_all.sh` and fix every failure before saying you are done. (A Stop hook also runs it.)
- Every behaviour change gets a test: unit test (`tests/test_*.cpp`), a simulation scenario (`tests/sim/test_cycle.cpp`) or a dashboard check (`tests/app/test_dashboard.js`).
- Bump the firmware version (`fw` in status, README "What's new", docs headers) together.
- Keep README, `docs/PROTOCOL.md`, `docs/TESTING.md` and the manuals in step with the code.

## Never
- Never edit `DashboardPage.h` by hand: edit `web/dashboard.html` / `web/es.json`, then `python3 tools/build_dashboard.py` (an edit hook runs it).
- Never write SSR pins outside `GreyWaterOutputs` (the only owner; interlocks live there). Pump and tap valve must never be on together.
- Never hold `gwLock` across network calls, `delay()` or anything blocking.
- No `delay()` in `loop()`; no dynamic `String`/heap churn in hot paths; ISRs only count pulses.
- No secrets (Wi-Fi, ntfy topics, passwords) in code or commits.
- Every screen text must fit 128 px in all four fonts and both languages (`tools/check_screen_text.py`); every dashboard text needs a Spanish entry in `web/es.json`.

## Code review
For reviews, audits or "is this ready to flash/release?" use the `arduino-review` skill (`/arduino-review`).
