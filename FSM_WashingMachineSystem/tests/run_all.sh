#!/bin/sh
# Runs every PC test for the grey water firmware. Usage: sh tests/run_all.sh
# Exit code 0 only if every suite passes.
# Optional: set ARDUINO_CLI to also compile the firmware for the Arduino Nano ESP32.
cd "$(dirname "$0")"
T=$(mktemp -d)
FAIL=0
run() { name="$1"; shift; out=$("$@" 2>&1); rc=$?; echo "$out" | tail -1; [ $rc -eq 0 ] || { echo "$out" | grep -E "FAIL|error" | head -20; echo "!! $name FAILED"; FAIL=1; }; }
echo "== Flow meter, step gates and pump protection =="
g++ -std=c++17 -O1 -Wall test_flow.cpp -o "$T/test_flow" && run flow "$T/test_flow"
echo "== Tank level switches =="
g++ -std=c++17 -O1 -Wall test_level.cpp -o "$T/test_level" && run level "$T/test_level"
echo "== Screen texts fit the OLED (both languages) =="
if command -v python3 >/dev/null 2>&1; then run screen python3 ../tools/check_screen_text.py; else echo "skipped: python3 not found"; fi
echo "== Every dashboard text has a Spanish translation (web/es.json) =="
if command -v python3 >/dev/null 2>&1; then run translations python3 ../tools/i18n_keys.py; else echo "skipped: python3 not found"; fi
echo "== Water history storage =="
mkdir -p "$T/h" && cp stubs/* "$T/h/" && cp ../GreyWaterSettings.cpp ../GreyWaterSettings.h "$T/h/"
g++ -std=c++17 -O1 -Wall -I"$T/h" test_history.cpp "$T/h/GreyWaterSettings.cpp" -o "$T/test_history" && run history "$T/test_history"
echo "== Full cycle simulation of the real firmware =="
run simulation sh sim/run_sim.sh
echo "== Dashboard in a headless browser (needs Node.js + Playwright; axe-core optional) =="
if command -v node >/dev/null 2>&1 && node -e "require('playwright')" >/dev/null 2>&1; then
  run dashboard node app/test_dashboard.js
else
  echo "skipped: Node.js with Playwright not found (npm i playwright axe-core; npx playwright install chromium)"
fi
echo "== Firmware 2.3 behaviour (reference, shows the old faults) =="
g++ -std=c++17 -O1 old_cycle_compare.cpp -o "$T/old" && "$T/old" | tail -3
if [ -n "$ARDUINO_CLI" ]; then
  echo "== Firmware build for the Arduino Nano ESP32 =="
  "$ARDUINO_CLI" compile --fqbn esp32:esp32:nano_nora .. | grep "Sketch uses"
fi
rm -rf "$T"
[ $FAIL -eq 0 ] && echo "All tests passed." || { echo "SOME TESTS FAILED."; exit 1; }
