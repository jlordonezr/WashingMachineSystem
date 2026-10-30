# Installation and configuration – firmware 3.2

## 1. Tools

| Item | Version |
|---|---|
| Arduino IDE 2.x (or arduino-cli 1.x) | any recent |
| Board package **esp32 by Espressif Systems** | **3.0.7** or **2.0.17** (both verified) |
| Board | **Arduino Nano ESP32**; Tools → Pin Numbering → **By Arduino pin (default)** |

Libraries (Library Manager): **WiFiManager** by tzapu, **Adafruit GFX Library**, **Adafruit SSD1306** (installs Adafruit BusIO), **AgileStateMachine**. Everything else comes with the board package. No new libraries in 3.0.

## 2. Open and build

Open `FSM_WashingMachineSystem/FSM_WashingMachineSystem.ino`. The folder name must stay `FSM_WashingMachineSystem`. Verify: the build should report about 1.37 MB (43%) of program storage and 95 KB (29%) of dynamic memory (3.2.0 with core 3.3.12).

Command line:

```
arduino-cli compile --fqbn esp32:esp32:nano_nora FSM_WashingMachineSystem
```

## 3. Upload

- **First time, or from 2.x over USB:** Upload with the board on USB.
- **Wireless (2.x already installed):** Tools → Port → *reusewater at <IP>*, Upload, enter the update password. The controller must be Ready with no tank wash running.
- **Name change:** boards running a build from before the rename appear as *greywater at <IP>*; after that upload they appear as *reusewater at <IP>* and the app moves to `http://reusewater.local`. The IP address, passwords, settings and history do not change.

**Buzzer (3.2):** the optional buzzer moved from D12 to **D1 (TX0)**. Move its signal wire before or right after the upload. A short chirp at power-on is normal: the chip sends its start-up messages on TX0 before the firmware takes the pin.

Settings and water history are kept. 3.1 upgrades settings version 6 to 7 by adding the language, which starts as Spanish; change it under Configuración → Idioma.

## 4. First-time setup (new board, or new Wi-Fi)

1. On first boot the screen shows `ReuseWater-Setup` and `Key gw-xxxxxx`. The key is unique to the board; it is also printed on the USB serial monitor (9600 baud). Without a working screen the key is `greywater`.
2. Join **ReuseWater-Setup** with that key on a phone. The setup page opens (or browse to `192.168.4.1`). Choose the home network.
3. The screen shows the controller's IP address for 6 s. Open `http://reusewater.local` or that address; sign in as `admin` / `greywater`.
4. The app asks for two new passwords (app and wireless updates). Both take effect at once.
5. **Settings → Language:** Español (default) or English, for the app, the screen and the alerts.
6. **Settings → Alerts:** *Create a private topic*, subscribe to it in the ntfy app, press **Send a test alert**. The app now reports whether it was delivered.

To open the setup page again (new router), hold **STOP for 5 s** while the screen shows `READY`. It closes after 3 minutes.

## 5. Check after installing

On the **Service** tab:

| Item | Expected |
|---|---|
| Firmware | 3.2.0 |
| Screen | Working |
| Tank level switches | Match the real tank; *Tank level* not "Switches disagree" |
| Slowest loop pass | Normally tens of ms. Note the value while the page loads on a weak-signal phone (see TESTING.md, H5) |
| Last alert | Delivered (after the test alert) |
| Recent events | `Controller started (…), firmware 3.2.0`, no `Slow loop pass` at idle |

Then run the hardware checks in `docs/TESTING.md` before the first real wash.

## 6. Editing the dashboard

The page source is `web/dashboard.html`; its Spanish texts are in `web/es.json` (English text → Spanish). After editing either:

```
python3 tools/i18n_keys.py          # lists any app text without a Spanish translation
python3 tools/check_screen_text.py  # checks that every screen text (TRS) fits the OLED in both languages
python3 tools/build_dashboard.py    # rebuilds DashboardPage.h (gzip, Spanish included)
sh tests/run_all.sh                 # includes the browser tests if Node.js + Playwright are installed
```

then upload the firmware.

## 7. Rolling back

| To | How | When |
|---|---|---|
| 3.0 | Upload `v3.0/FSM_WashingMachineSystem` from the previous archive (English only) | 3.1 misbehaves |
| 2.9.3 | Upload `v2.9.3/FSM_WashingMachineSystem` | 3.0's safety task misbehaves on the hardware. 2.9.3 has all other 3.0 fixes |
| 2.9 | Upload the original `v2.9` folder | Anything else |

History is the same in every version. Going back from 3.1 to 3.0 or earlier resets the settings to their defaults once (those versions do not read settings version 7); note them first, or restore them from a downloaded history file.

## 8. Running the PC tests

```
sh tests/run_all.sh
```

Needs `g++`. The simulation needs the AgileStateMachine library in `~/Arduino/libraries` (or set `AGILE` to its `src` folder). The browser tests need Node.js with `playwright` (and optionally `axe-core`); without them that suite is skipped.
