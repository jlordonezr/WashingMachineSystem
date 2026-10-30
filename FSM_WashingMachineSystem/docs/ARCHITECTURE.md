# Architecture – firmware 3.1

One Arduino Nano ESP32 runs everything: the wash-cycle state machine, the sensors, five SSR outputs, the OLED, and a web app served from the board. There is no separate server, database or phone app; the dashboard is one page stored in the firmware.

## Hardware (unchanged since 2.1)

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
| D12 | — | Optional active buzzer | `BUZZER_PIN` |
| I2C 0x3C | — | SSD1306 128×32 OLED, 400 kHz | `oled` |

SSRs are on when their pin is HIGH (`ssrON`). Wiring details are in the Runbook.

## Tasks

| Task | Core | Priority | Period | Owns |
|---|---|---|---|---|
| Safety (`GreyWaterTask.cpp`) | 1 | 3 | 20 ms | Float switches, pump cut-off, interlocks, SSR pins |
| `loop()` (Arduino) | 1 | 1 | as fast as it runs | Cycle, buttons, flow meters, screen, statistics, web server, OTA, event log |
| Alerts (`GreyWaterNet.cpp`) | 0 | 1 | on demand | HTTPS POSTs to ntfy, from a queue of 6 |
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
├─ GreyWaterSensors     flow interrupts + FlowMeter (rate, flowing, starts, litres)
├─ GreyWaterDisplay     OLED: status / notices, frame-timed, only redraws on change
├─ GreyWaterDiagnostics event log (50, RAM), loop timing, buzzer, restart reason
├─ GreyWaterSettings    settings + history in NVS (versioned structs, history twice + kept-aside copy)
└─ GreyWaterNet         Wi-Fi (WiFiManager), web server + API, OTA, mDNS, NTP, alert task
     └─ DashboardPage.h  gzip of web/dashboard.html (tools/build_dashboard.py)

Hardware-free, unit tested: FlowMeter.h (FlowMeter, CycleGates, PumpGuard), TankLevel.h
```

## Output modes and interlocks

The cycle requests one of these modes; `GreyWaterOutputs` writes the pins.

| Mode | Used in | Tap D6 | Drain D4 | Fill D5 | Pump D7 | Wash D0 |
|---|---|---|---|---|---|---|
| `modeOff` | boot, update, restart | off | off | off | off | off |
| `modeStandby` | Ready, Starting, not saving | on | on | off | off | off |
| `modeTapFill` | step 1 (tap), step 3 | on | on | off | off | off |
| `modeTankFill` | step 1 (tank water) | off | on | off | on | pulses (2 s every 10 s) |
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
app ─HTTP─► web server (loop) ─► requestRemoteStart/Stop flags ─► applied at the start of the next loop pass
cycle (loop) ─► outputsRequest(mode) ─► GreyWaterOutputs (lock) ─► SSR pins
status snapshot (loop) ─► /api/status JSON ─► app (polls every 2 s)
events ─► notify() ─► queue ─► alert task ─► ntfy ─► result queue ─► event log + /api/status.alert
```

## Why the app reflects the real hardware

- `outputs` in the status is the pin state actually written, after interlocks.
- Start, Stop and Wash tank are shown as done only when a later status shows the new state; the POST reply only means "accepted".
- The test alert is reported as delivered only when the alert server answered 2xx.
- Tank level and the switch fault come from the same debounced readings the interlocks use.

## Languages (3.1)

`cfg.language` (`LANG_ES` default, `LANG_EN`) picks every text at the moment it is shown: `TRS("en", "es")` for screen texts (plain ASCII, checked to fit), `TR("en", "es")` for app sentences and alerts (UTF-8). The status JSON carries `lang`; the app translates its own texts with `web/es.json` and follows the controller's setting. The event log and API error messages stay English (the app translates the errors).

## Storage

NVS namespace `greywater`: `cfg` (Settings v7 from 3.1, adds `language`), `stats` and `stats_bak` (WaterStats v3), `stats_keep` (an unreadable record kept aside), `cyc` (cycle-in-progress marker). Settings v6 (3.0) is upgraded to v7 at start-up; history is unchanged.
