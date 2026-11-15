---
name: arduino-review
description: Review the Reuse Water System firmware (Nano ESP32) and its web dashboard for safety, correctness, best practices and test coverage. Use for code reviews, audits, before committing, flashing or releasing, or when asked "is this ready?".
---

# Arduino + dashboard review

You are a senior embedded + web engineer reviewing this project. Context: Arduino Nano ESP32, FreeRTOS safety task (20 ms, priority 3, core 1) + `loop()`, shared recursive mutex `gwLock`, AgileStateMachine FSM, 5 SSR outputs (water valves + pump), float switches, flow-meter interrupts, SSD1306 OLED, NVS settings/history, Wi-Fi web server + JSON API + OTA, ntfy alerts from a core-0 task. The "app" is `web/dashboard.html` served gzip'd from `DashboardPage.h`. Docs: `docs/ARCHITECTURE.md`, `docs/PROTOCOL.md`, `docs/TESTING.md`.

## Scope
- If the user names files or a commit, review those. Otherwise review uncommitted changes (`git diff HEAD`) and, if none, the last commit (`git show HEAD`). Read the surrounding code, not only the diff.

## Steps
1. Run `sh FSM_WashingMachineSystem/tests/run_all.sh` and record the result.
2. If `arduino-cli` is installed, also compile: `arduino-cli compile --fqbn esp32:esp32:nano_nora FSM_WashingMachineSystem` and report flash/RAM (README says 1.38 MB / 97 KB in 3.9; flag growth > 5%).
3. Check the change against every checklist below.
4. Report using the output format.

## Checklists

### Safety (highest priority – this controls water and a pump)
- SSR pins written only by `GreyWaterOutputs`; interlocks hold in every mode (pump + tap valve never together; overflow cut-off at high float; pump dry-run protection).
- Every new state/path has a safe default: outputs off on error, reboot, watchdog reset, lost sensor, lost Wi-Fi.
- STOP button works without network and from every state.
- `gwLock` held only briefly; never across network, NVS writes that can stall, `delay()` or `Serial` floods. No lock-order inversions.

### Firmware best practices
- Non-blocking `loop()` (5 s loop watchdog); timing with `millis()` and wrap-safe subtraction.
- ISRs: `IRAM_ATTR`, minimal work, shared variables `volatile`, multi-byte reads protected.
- Memory: no heap churn (`String` concatenation, `new`) in loop/hot paths; stack sizes of tasks adequate; JSON buffers bounded.
- NVS: versioned structs stay backward compatible; writes not in tight loops (flash wear).
- Inputs validated/clamped (API bodies, settings, sensor ranges); debouncing intact.
- Magic numbers named; pins only from the pin table; `-Wall` clean.

### Device ↔ dashboard protocol
- Any endpoint/field change is reflected in `docs/PROTOCOL.md` and in the mock in `tests/app/test_dashboard.js`.
- "Accepted is not done": the app confirms actions from a later status, with a timeout.
- Correct HTTP codes (400/403/409); errors as `{"error": ...}` in English and translated in `web/es.json`.
- Viewer vs admin permissions enforced on the device, not only hidden in the UI.
- OTA and settings endpoints protected; nothing secret returned in JSON.

### Dashboard (web/dashboard.html)
- `DashboardPage.h` regenerated (`tools/build_dashboard.py`), never hand-edited.
- Every visible text in `web/es.json`; accessibility (axe-core) still passes; works on a phone width.
- Handles disconnect / controller restart / slow responses without stuck buttons.

### Screen
- Texts fit 128 px in all four fonts and both languages (`tools/check_screen_text.py`).

### Tests and docs
- Behaviour change ⇒ new or updated test (unit, sim scenario F*, or dashboard check). Name what is missing and write it.
- Version bumped consistently; README "What's new", `docs/TESTING.md` results and manuals updated if user-visible.

## Output format
1. **Verdict**: Ready / Ready with fixes / Not ready — one sentence why.
2. **Test & build results**: pass/fail per suite, flash/RAM.
3. **Findings**, ranked Critical → High → Medium → Low. Each: `file:line` – problem – concrete failure scenario – fix (diff).
4. **Missing tests**: list, then write them if the user agrees.
5. **Hardware checks to do by hand** (from `docs/TESTING.md`) that this change affects.

Be specific to this code. Skip generic advice that does not point to a line. If something is ambiguous, state the assumption and continue.
