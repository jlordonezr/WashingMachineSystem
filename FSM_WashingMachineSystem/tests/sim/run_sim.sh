#!/bin/sh
# Builds the real firmware for the PC simulation and runs every scenario.
# GreyWaterTask.cpp (FreeRTOS) is replaced by stand-ins in test_cycle.cpp, which
# calls safetyTick() every 20 ms of simulated time, as the task does on the board.
# Needs the AgileStateMachine library: set AGILE to its src folder if it is elsewhere.
set -e
cd "$(dirname "$0")"
AGILE=${AGILE:-$HOME/Arduino/libraries/AgileStateMachine/src}
B=$(mktemp -d)
cp ../../FSM_WashingMachineSystem.ino "$B/sketch.cpp"
g++ -std=gnu++17 -O1 -w -I. -I../.. -I"$AGILE" -o "$B/test_cycle" test_cycle.cpp "$B/sketch.cpp" \
    ../../GreyWaterSystem.cpp ../../GreyWaterSettings.cpp ../../GreyWaterOutputs.cpp ../../GreyWaterSensors.cpp \
    ../../GreyWaterDisplay.cpp ../../GreyWaterDiagnostics.cpp "$AGILE"/*.cpp
FAIL=0
for s in F1 F2 F3 F4a F4b F5 F6 F7 F8 F9 F10 F11 F12; do "$B/test_cycle" $s "$B/flash.bin" || FAIL=1; done
rm -rf "$B"
[ $FAIL -eq 0 ] && echo "All simulation scenarios passed." || { echo "Simulation FAILED."; exit 1; }
