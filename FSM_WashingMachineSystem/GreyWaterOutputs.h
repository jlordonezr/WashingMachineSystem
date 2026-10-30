#ifndef GREYWATER_OUTPUTS_H
#define GREYWATER_OUTPUTS_H

// The five SSR outputs, the tank float switches and the safety interlocks.
//
// Firmware 3.0 (finding F1 of the 2.9 assessment): in 2.9 every valve and pump
// decision ran inside loop(), together with the web server. A slow phone could
// hold loop() for seconds, and during that time the pump cut-off (300 ms) and
// the tank-full checks did not run.
//
// Now this module is the ONLY code that writes the SSR pins, and it does so
// behind one lock (gwLock). The cycle in loop() only REQUESTS a mode. A separate
// high-priority task calls safetyTick() every SAFETY_TICK_MS, whatever loop() is
// doing, and enforces the interlocks:
//
//   - fill valve (rinse water to the tank): never open while the high float is wet
//   - tank-wash valve: never open while the high float is wet
//   - pump: never on while the low float is dry; stops within PUMP_NO_FLOW_MS
//     when the machine stops drawing (PumpGuard, FlowMeter.h)
//
// When an interlock overrides the requested mode, the outputs fall back to the
// same safe states 2.9 used: tap fill instead of the pump, "not saving" (drain
// open) instead of saving to the tank.

#include <Arduino.h>
#include "FlowMeter.h"
#include "TankLevel.h"

#define SAFETY_TICK_MS 20      // safety task period

// What the relays are doing; also used to attribute measured water volumes.
enum RelayMode {
  modeOff,         // everything off (boot, firmware update)
  modeStandby,     // Idle / Run / not saving: tap + drain on
  modeTapFill,     // filling the machine from the tap
  modeTankFill,    // filling the machine from the tank (pump on, tank-wash valve pulsing)
  modeDrainSoapy,  // step 2
  modeSaveTank,    // step 4, rinse water into the tank
  modeManualWash   // Idle outputs plus the tank-wash valve held open
};

// Things the safety task did that loop() should log or announce.
struct SafetyEvents {
  bool     pumpStopped;      // PumpGuard switched the pump off
  bool     pumpStoppedDry;   // ... and no water had flowed since it started
  uint32_t dryStarts;        // dry starts in this cycle
  bool     washClosedFull;   // the high float closed the tank-wash valve during a manual wash
};

// Lock shared by loop() and the safety task (GreyWaterTask.cpp on the board,
// a no-op in the PC simulation). Hold it only for short, non-blocking work.
void gwLock();
void gwUnlock();
struct GwLockGuard { GwLockGuard() { gwLock(); } ~GwLockGuard() { gwUnlock(); } };

void      outputsBegin();                   // pins as outputs, everything off (call early in setup)
void      safetyBegin();                    // start the safety task (call at the end of setup)
void      safetyTick();                     // one safety pass; the task calls it every SAFETY_TICK_MS

void      outputsRequest(RelayMode mode);   // what the cycle wants; interlocks may override it
RelayMode outputsMode();                    // what the outputs actually do (after interlocks)
uint8_t   outputsBits();                    // bit0 tap, 1 drain, 2 fill, 3 pump, 4 tank wash
bool      outputsWashOpen();
void      outputsAllOff();                  // all SSRs off (boot, firmware update, restart)
void      resetTankWash();                  // next tank-water fill starts with a wash-valve opening

// Pump (PumpGuard, FlowMeter.h). The fill logic asks every 250 ms; the safety
// task does the fast cut-off.
bool      pumpAllow(bool wantTankWater);    // true: fill from the tank now
void      pumpResetForStep();               // a new step begins
void      pumpResetForCycle();              // a new cycle begins (clears dry-start count)
void      pumpNoteFlowStarts(uint32_t tapStarts);   // loop() passes the tap meter's start count
bool      safetyTakeEvents(SafetyEvents& e);         // true if something happened since the last call

// Tank float switches (debounced in the safety task, TankLevel.h).
bool      tankHasWater();      // low float wet: the pump may draw from the tank
bool      tankFull();          // high float wet: nothing may add water to the tank
bool      tankFault();         // high wet but low dry for a while: check the switches
bool      tankHighRawWet();    // high float right now, not debounced (manual wash closes at once)
uint8_t   tankLevelRaw();      // TankLevel::Level (0 low, 1 medium, 2 high)

#endif
