# Architecture – firmware 3.18

One Arduino Nano ESP32 runs everything: the wash-cycle state machine, the sensors, five SSR outputs, the OLED, and a web app served from the board. There is no separate server, database or phone app; the dashboard is one page stored in the firmware.

## Hardware (buzzer moved to D1 in 3.2; otherwise unchanged since 2.1)

| Pin | GPIO | Function | Code name |
|---|---|---|---|
| D2 | 5 | Tap flow sensor YF-B1, 660 pulses/L, interrupt | `TAP_PIN` |
| D3 | 6 | Drain flow sensor YF-B10, 352 pulses/L, interrupt | `DRAIN_PIN` |
| D4 | 7 | Drain valve SSR | `OUT_DRAIN_EV` |
| D5 | 8 | Fill valve SSR (rinse water to the tank) | `OUT_FILL_EV` |
| D6 | 9 | Tap valve SSR | `OUT_TAP_EV` |
| D7 | 10 | Tank pump SSR | `OUT_MOTOR` |
| D0 / RX0 | 44 | Tank-wash valve SSR (10 kΩ to GND) | `OUT_WASH_EV` |
| D8 | 17 | Tank high-level float, HIGH = wet (external pull-down) | `IN_HIGHLEVEL` |
| D9 | 18 | Tank low-level float, HIGH = wet (external pull-down) | `IN_LOWLEVEL` |
| D10 | 21 | STOP button, active LOW, internal pull-up | `STOP_BUTTON` |
| D11 | 38 | START button, active LOW, internal pull-up | `START_BUTTON` |
| D1 / TX0 | 43 | Optional active buzzer (do not use `Serial0`; may chirp briefly at power-on) | `BUZZER_PIN` |
| I2C 0x3C (or 0x3D) | — | SSD1306 128×64 OLED, rows 0-15 yellow and 16-63 blue (3.12), 400 kHz | `oled` |

SSRs are on when their pin is HIGH (`ssrON`). Wiring details are in the Runbook.

## Tasks

| Task | Core | Priority | Period | Owns |
|---|---|---|---|---|
| Safety (`GreyWaterTask.cpp`) | 1 | 3 | 20 ms | Float switches, pump cut-off, interlocks, SSR pins |
| `loop()` (Arduino) | 1 | 1 | as fast as it runs | Cycle, buttons, flow meters, screen, statistics, web server, OTA, event log |
| Alerts (`GreyWaterNet.cpp`) | 0 | 1 | on demand | HTTPS POSTs to ntfy (message built in `Ntfy.h`), from a queue of 6; failed alerts wait in 6 slots and are tried again for up to 30 min (3.3.1) |
| Wi-Fi / lwIP (ESP-IDF) | 0 | high | — | Network stack |
| Flow interrupts | — | ISR | per pulse | Pulse counters only (1 ms glitch filter) |

The safety task and `loop()` share one recursive mutex (`gwLock`). It is held only for short, non-blocking work: never across network calls. If either side ever deadlocked, `loop()` would stop and the 5 s loop watchdog would restart the board with every output off; the interrupted-cycle check then reports it.

## Module map

```
FSM_WashingMachineSystem.ino   setup(), loop(), state machine wiring (AgileStateMachine)
│
├─ GreyWaterSystem      cycle: step logic, manual tank wash, statistics, tank age, status snapshot
│    requests modes ──► GreyWaterOutputs   only writer of the SSR pins; interlocks; PumpGuard; TankLevel
│                         ▲  safetyTick() every 20 ms from GreyWaterTask (FreeRTOS)
├─ TankVolume.h       estimated tank litres: flow in/out, wash water at an assumed rate, float corrections (3.13)
├─ GreyWaterSensors     flow interrupts + FlowMeter (rate, flowing, starts, litres)
├─ GreyWaterDisplay     OLED 128x64: header in the yellow rows (title, tank litres, arrow, gauge; 3.13), message on one or two blue lines ("top|bottom"); start-up animation: Border Collie chasing a butterfly (BootAnimation.h), frame-timed, only redraws on change
├─ GreyWaterDiagnostics event log (50, RAM), loop timing, buzzer, restart reason
├─ GreyWaterSettings    settings + history in NVS (versioned structs, history twice + kept-aside copy)
└─ GreyWaterNet         Wi-Fi (WiFiManager; power saving off, IPv6 on, 3.16), web server + API, OTA, mDNS, NTP, alert task
     ├─ HttpConnPool.h   waiting room for connections that have not sent a request yet (3.16)
     └─ DashboardPage.h  gzip of web/dashboard.html (tools/build_dashboard.py)

Hardware-free, unit tested: FlowMeter.h (FlowMeter, CycleGates, PumpGuard), TankLevel.h, HttpConnPool.h
```

## Output modes and interlocks

The cycle requests one of these modes; `GreyWaterOutputs` writes the pins.

| Mode | Used in | Tap D6 | Drain D4 | Fill D5 | Pump D7 | Wash D0 |
|---|---|---|---|---|---|---|
| `modeOff` | boot, update, restart | off | off | off | off | off |
| `modeStandby` | Ready, Starting, not saving | on | on | off | off | off |
| `modeTapFill` | step 1 (tap), step 3 (tap; in Reuse all only when the tank is low) | on | on | off | off | off |
| `modeTankFill` | step 1 (tank water); step 3 in Reuse all (3.18) | off | on | off | on | pulses (2 s every 10 s) |
| `modeDrainSoapy` | step 2 | on | on | off | off | off |
| `modeSaveTank` | step 4 | on | off | on | off | off |
| `modeManualWash` | Ready + Wash tank | on | on | off | off | on |

Interlocks, applied on every request and every 20 ms:

| Condition | Requested | Applied instead |
|---|---|---|
| Low float dry (debounced) | `modeTankFill` | `modeTapFill` (pump off, tap on) |
| High float wet (debounced) | `modeSaveTank` | `modeStandby` (fill closed, drain open) |
| High float wet (debounced or raw) | `modeManualWash` | `modeStandby` (wash valve closed) |
| High float wet (debounced or raw) | wash pulses in `modeTankFill` | wash valve stays closed |
| No tap pulse for 300 ms after the 1 s start grace | `modeTankFill` | `modeTapFill` until the machine starts a new draw (PumpGuard) |

Float debouncing (`TankLevel.h`): the safe reading counts after 0.2 s (high wet, low dry), the other after 2 s. "High wet, low dry" for 3 s is reported as a switch fault.

## Data flow

```
flow sensors ─ISR─► pulse counters ─► FlowMeter (loop) ─► CycleGates ─► step transitions
                         └──────────────► PumpGuard (safety task, 20 ms) ─► pump off
float switches ─────────────────────────► TankLevel (safety task) ─► interlocks, cycle decisions, screen, app
buttons ─► updateInputs (loop) ─► inStart / inStop ─► state machine
START press edge (3.17) ─► Ready: inStart ("Reuse and save") · Starting and steps 1-3: cycleMode moved on (3 modes, 3.18) ─► saveCycleSnapshot() ─► status / app
app ─HTTP─► web server (loop) ─► requestRemoteStart/Stop flags ─► applied at the start of the next loop pass
app ─HTTP─► /api/interrupted/resume ─► requestResume flag ─► takeResumeRequest (loop) ─► the sketch enters the saved step (3.10)
cycle (loop) ─► outputsRequest(mode) ─► GreyWaterOutputs (lock) ─► SSR pins
status snapshot (loop) ─► /api/status JSON ─► app (polls every 2 s, one request at a time, paused while hidden)
events ─► notify() ─► queue ─► alert task ─► ntfy ─► result queue ─► event log + /api/status.alert
```

## Cycle modes (3.18)

`cycleMode` in `GreyWaterSystem.cpp` is a `CycleMode` (until 3.17 the bool `cycleNoSave`):

| Mode | Value | Step 1 | Step 3 | Step 4 |
|---|---|---|---|---|
| `cmReuseSave` *Reuse and save* | 0 | tank water, tap when low | tap | rinse to the tank (`modeSaveTank`) |
| `cmReuseOnly` *Reuse only* | 1 | tank water, tap when low | tap | rinse to the drain (`modeStandby`) |
| `cmReuseAll` *Reuse all* | 2 | tank water, tap when low | tank water, tap when low | rinse to the drain (`modeStandby`) |

`cycleSaves()` (Reuse and save) decides step 4 and the tank-full alert; `cycleReusesInRinse()` (Reuse all) makes `fill2StateLogic()` call the same `checkAndFillWashingMachine()` as step 1 on every 250 ms pass, otherwise it calls `pumpAllow(false)` and asks for tap water. The interlocks and PumpGuard apply as in step 1, and the step transitions (`gates.fill2Done`) do not depend on the water source. The mode is chosen with `POST /api/start` `mode` or by START presses, stored in the cycle snapshot and, at the end of a wash, in `WaterStats::lastMode`.

## START button (3.17)

`readStartButton()` in `GreyWaterSystem.cpp` counts a press on the debounced press edge, only when START has been seen released since power-up (`startArmed`, so a button held at power-up never counts) and at least `START_MIN_GAP_MS` (1 s) after the last counted press (bounce). At Ready a press starts *Reuse and save* at once. While starting and in steps 1-3 each press moves `cycleMode` on (Reuse and save → Reuse only → Reuse all → Reuse and save, 3.18), writes the cycle snapshot and logs *Mode changed: … (panel)*; in step 4 (`stDrain2`) it only shows *Mode fixed*. Each counted press beeps once and shows the mode for `START_NOTICE_MS` (3 s). Held `START_STUCK_MS` (15 s): logged and shown as stuck, in any state. Only `millis()` and statics, no heap.

## Web server (3.16)

The stock ESP32 `WebServer` runs in `loop()` and serves one connection at a time. It used to wait up to 5 s for a connection that was open but silent, with every other request queued behind it; phones leave such connections open often, which made the app time out during washes. `GwWebServer` (in `GreyWaterNet.cpp`) accepts every waiting connection into `HttpConnPool` (4 places) and hands the stock code only a connection whose request has arrived, so one call answers one request and returns. Handlers only read values the cycle already keeps (no sensor or I2C reads) and send from stack buffers. The cycle, the safety task and the STOP button do not depend on the network at all.

## Why the app reflects the real hardware

- `outputs` in the status is the pin state actually written, after interlocks.
- Start, Stop and Wash tank are shown as done only when a later status shows the new state; the POST reply only means "accepted".
- The test alert is reported as delivered only when the alert server answered 2xx.
- Tank level and the switch fault come from the same debounced readings the interlocks use.

## Languages (3.1)

`cfg.language` (`LANG_ES` default, `LANG_EN`) picks every text at the moment it is shown: `TRS("en", "es")` for screen texts (plain ASCII, checked to fit), `TR("en", "es")` for app sentences and alerts (UTF-8). The status JSON carries `lang`; the app translates its own texts with `web/es.json` and follows the controller's setting. The event log and API error messages stay English (the app translates the errors).

## Storage

NVS namespace `greywater`: `tankL` (estimated tank litres, float, 3.13), `cfg` (Settings v7 from 3.1, adds `language`), `stats` and `stats_bak` (WaterStats v4 from 3.18: adds `lastMode` in the end padding, same size; v3 is migrated with the mode unknown), `stats_keep` (an unreadable record kept aside), `cyc` (cycle snapshot from 3.10: `CycleSnapshot`, 24 bytes, version `CYCLE_SNAP_VERSION` 2 from 3.18 with the cycle mode in byte 2; version 1 kept noSave 0/1 there and is still read; written at Start, at each step and when the machine has filled in step 1 or 3, removed when the cycle ends; read and removed by `bootChecks()` at power-up. The 8-byte marker of 2.9–3.9 is still recognised: reported, not resumable), `authkey` (3.11: 32 random bytes that sign the `gwsid` sign-in cookies, see `AuthToken.h`; made at the first sign-in, replaced by *Sign out other devices*). Settings v6 (3.0) is upgraded to v7 at start-up; history is unchanged.
