#!/bin/sh
# Builds the real firmware for the PC simulation and runs every scenario.
# GreyWaterTask.cpp (FreeRTOS) is replaced by stand-ins in test_cycle.cpp, which
# calls safetyTick() every 20 ms of simulated time, as the task does on the board.
# Needs the AgileStateMachine library: set AGILE to its src folder if it is elsewhere.
set -e
cd "$(dirname "$0")"
if [ -z "$AGILE" ]; then   # Arduino IDE sketchbook: ~/Arduino (Linux, Windows) or ~/Documents/Arduino (macOS)
  AGILE=$HOME/Arduino/libraries/AgileStateMachine/src
  [ -d "$AGILE" ] || AGILE=$HOME/Documents/Arduino/libraries/AgileStateMachine/src
fi
B=$(mktemp -d)
cp ../../FSM_WashingMachineSystem.ino "$B/sketch.cpp"
g++ -std=gnu++17 -O1 -w -DTANK_CAPACITY_L=45.0f -DTANK_LOW_MARK_L=10.0f -DTANK_HIGH_MARK_L=40.0f -I. -I../.. -I"$AGILE" -o "$B/test_cycle" test_cycle.cpp "$B/sketch.cpp" \
    ../../GreyWaterSystem.cpp ../../GreyWaterSettings.cpp ../../GreyWaterOutputs.cpp ../../GreyWaterSensors.cpp \
    ../../GreyWaterDisplay.cpp ../../GreyWaterDiagnostics.cpp "$AGILE"/*.cpp
FAIL=0
for s in F1 F2 F3 F4a F4b F5 F6 F7 F8 F9 F10 F11 F12 F13 F14 F15 F16a F16b F17 F18 F19 F20 F21a F21b F22 F23 F24 F25 F26 F27a F27b F28 F29 F30 F31 F32 F33 F34 F35 F36a F36b F37 F38; do "$B/test_cycle" $s "$B/flash.bin" || FAIL=1; done
rm -rf "$B"
[ $FAIL -eq 0 ] && echo "All simulation scenarios passed." || { echo "Simulation FAILED."; exit 1; }
