# Grey Water Washing Machine System

Firmware for a grey-water controller that sits beside a domestic washing machine. An Arduino Nano ESP32 follows the machine's wash program, refills the machine from a storage tank instead of the tap, and saves the rinse water back to the tank for the next wash. A web app on the home Wi-Fi shows the cycle, the tank and the water saved, and phone alerts (ntfy) report problems.

**Current version: 3.1.0** (Spanish and English; Spanish by default).

## Repository layout

| Path | Contents |
|---|---|
| `FSM_WashingMachineSystem/` | The Arduino sketch (open `FSM_WashingMachineSystem.ino`). Its `README.md` has the full change history |
| `FSM_WashingMachineSystem/docs/` | Architecture, communication protocol, installation, testing |
| `FSM_WashingMachineSystem/tests/` | PC unit tests, full-cycle simulation of the real firmware, browser tests of the app |
| `FSM_WashingMachineSystem/web/` | App page source (`dashboard.html`) and Spanish texts (`es.json`) |
| `FSM_WashingMachineSystem/tools/` | Page builder and the screen-text and translation checks |
| `manuals/` | Runbook (installers and service) and Quick Card (household), PDF |

## Hardware

Arduino Nano ESP32, five SSRs (tap, drain and fill valves, tank pump, tank-wash valve), two flow sensors (YF-B1 tap, YF-B10 drain), two tank float switches, START and STOP buttons, SSD1306 128×32 OLED, optional buzzer. Pin table and wiring: `FSM_WashingMachineSystem/docs/ARCHITECTURE.md` and the Runbook.

## Build

Arduino IDE 2 or arduino-cli, board package **esp32 by Espressif** 2.0.17 or 3.0.7, board **Arduino Nano ESP32**, pin numbering *By Arduino pin*. Libraries: WiFiManager (tzapu), Adafruit GFX, Adafruit SSD1306, AgileStateMachine. Details: `FSM_WashingMachineSystem/docs/INSTALL.md`.

## Tests

```
sh FSM_WashingMachineSystem/tests/run_all.sh
```

Needs `g++` and Python 3; the simulation needs AgileStateMachine in `~/Arduino/libraries`; the app tests need Node.js with Playwright (axe-core optional). See `FSM_WashingMachineSystem/docs/TESTING.md`.

## Versions

| Version | Highlights |
|---|---|
| 3.1.0 | Spanish (Colombia) and English for the screen, app and alerts; alerts sent as ntfy JSON |
| 3.0.0 | Safety task and output interlocks, float-switch rules, per-board Wi-Fi setup key, app shows only confirmed actions, code split into modules. Tested on the installed system |
| 2.x | See `FSM_WashingMachineSystem/README.md` |
