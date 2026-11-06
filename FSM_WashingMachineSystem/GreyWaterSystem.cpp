#include "GreyWaterSystem.h"
#include "GreyWaterNet.h"
#include "GreyWaterSettings.h"
#include "GreyWaterOutputs.h"
#include "GreyWaterSensors.h"
#include "GreyWaterDiagnostics.h"
#include <time.h>

// The wash cycle: buttons, step logic, manual tank wash, water statistics,
// tank water age and the status snapshot for the dashboard. Runs in loop().
//
// Firmware 3.0 moved the other parts out of this file (no behaviour change):
//   GreyWaterOutputs.cpp     SSR outputs, float switches, safety interlocks, pump cut-off
//   GreyWaterTask.cpp        the safety task and its lock (FreeRTOS)
//   GreyWaterSensors.cpp     flow-sensor interrupts and meters
//   GreyWaterDisplay.cpp     OLED drawing
//   GreyWaterDiagnostics.cpp event log, loop timing, buzzer, restart reason

static CycleGates gates;                  // what the current step waits for (FlowMeter.h)
static bool       pumpDryAlertSent = false;
static bool       tankFaultReported = false;

// ── Interrupted cycles ─────────────────────────────────────────────────────
static bool     interruptedFlag  = false;
static int      interruptedState = 0;
static uint32_t interruptedEpoch = 0;
static bool     interruptedResumable = false;   // the full snapshot was found (3.10)
static CycleSnapshot interruptedSnap;           // what the cut cycle had done
static unsigned long interruptedAtMs = 0;       // millis() when it was found (start-up)

bool interruptedInfo(int& state, uint32_t& epoch) { state = interruptedState; epoch = interruptedEpoch; return interruptedFlag; }
void dismissInterrupted() { interruptedFlag = false; }
bool interruptedNoSave() { return interruptedFlag && interruptedSnap.noSave; }

void bootChecks() {
  logEvent("Controller started (%s), firmware %s", resetReasonText(), FIRMWARE_VERSION);
  if (!displayAvailable()) {
    logEvent("Screen not found: running without it");
    notify(TR("Reuse Water: screen not working", "Reúso de Agua: la pantalla no responde"),
           TR("The controller screen did not answer at start-up. The wash cycle, buttons and app work as normal. "
              "Check the screen's wiring (I2C, address 0x3C).",
              "La pantalla del controlador no respondió al encender. El ciclo, los botones y la app funcionan normalmente. "
              "Revise el cableado de la pantalla (I2C, dirección 0x3C)."), 3, "warning");
  }
  CycleSnapshot snap;
  CycleSnapFound found = cycleSnapLoad(snap);
  if (found != snapNone) {
    int st = snap.state < STATE_COUNT ? snap.state : stIdle;
    interruptedFlag = true; interruptedState = st; interruptedEpoch = snap.epoch;
    interruptedSnap = snap; interruptedSnap.state = (uint8_t)st;
    interruptedResumable = found == snapFull && st >= stRun;   // a 3.9 marker has no mode or litres
    interruptedAtMs = millis();
    cycleMarkEnd();          // the snapshot is kept in RAM; Resume writes a new one
    logEvent("Cycle interrupted in step %d (%s)", st >= stFill1 ? st - 1 : 0, resetReasonText());
    if (interruptedResumable) logEvent("Resume offered in the app for %lu min", (unsigned long)(RESUME_OFFER_MS / 60000UL));
    displayShowNotice(TRS("INTERRUPTED", "INTERRUMPIDO"),
                      interruptedResumable ? TRS("Check washer|Resume in app", "Revise lavadora|Reanude en app")
                                           : TRS("Check washer|Start again", "Revise lavadora|Inicie de nuevo"), 15000);
    char body[320];
    if (interruptedResumable)
      snprintf(body, sizeof(body), TR("The wash cycle was cut short (%s). The controller is back to Ready. "
                                      "Check the washing machine, then resume the cycle from the app within %lu minutes, or start a new one.",
                                      "El ciclo de lavado se interrumpió (%s). El controlador volvió a Listo. "
                                      "Revise la lavadora y reanude el ciclo desde la app en los próximos %lu minutos, o inicie uno nuevo."),
               resetReasonTextLocal(), (unsigned long)(RESUME_OFFER_MS / 60000UL));
    else
      snprintf(body, sizeof(body), TR("The wash cycle was cut short (%s). The controller is back to Ready. "
                                      "Check the washing machine; start a new cycle if needed.",
                                      "El ciclo de lavado se interrumpió (%s). El controlador volvió a Listo. "
                                      "Revise la lavadora; inicie un ciclo nuevo si hace falta."), resetReasonTextLocal());
    notify(TR("Reuse Water: cycle interrupted", "Reúso de Agua: ciclo interrumpido"), body, 4, "warning");
    beep(BEEP_ALARM);
  }
}

// ── Button debounce ────────────────────────────────────────────────────────
bool debounceButton(int pin, unsigned long& lastPressTime, bool& lastState) {
  bool current = digitalRead(pin);
  if (current != lastState) {
    lastPressTime = millis();
    lastState = current;
  }
  return (millis() - lastPressTime > DEBOUNCE_DELAY) && (current == LOW);
}

// START on the panel (3.8). At READY the cycle starts when START is released:
// a press shorter than START_HOLD_MS starts "Reuse and save", a longer one
// "Reuse only". While it is held the screen shows which one a release starts,
// so nobody has to count. Held START_STUCK_MS or more: treated as a stuck
// button and nothing starts. In any other state START does nothing, as before.
static bool startNoSave = false;        // the next Start is "Reuse only" (panel hold or app)
static bool startDown   = false;        // START held (debounced) on the last pass
static bool startStuck  = false;
static bool startLong   = false;        // held past START_HOLD_MS
static unsigned long startDownSince = 0;

static void showStartChoice(bool reuseOnly) {
  displayShowNotice(reuseOnly ? TRS("REUSE ONLY", "SOLO REUSAR") : TRS("REUSE AND SAVE", "REUSAR Y GUARDAR"),
                    TRS("Release to start", "Suelte e inicia"), START_STUCK_MS + 1000);
}

static void readStartButton() {
  bool down = debounceButton(START_BUTTON, lastStartButtonPressTime, lastStartButtonState);
  inStart = false;
  if (down) {
    if (!startDown) {                   // just pressed
      startDown = true;
      startDownSince = millis();
      startLong = startStuck = false;
      if (activeState == stIdle) {
        displayClearNotice();           // also ends the start-up animation
        showStartChoice(false);
      }
    }
    unsigned long held = millis() - startDownSince;
    if (activeState != stIdle || startStuck) return;
    if (held >= START_STUCK_MS) {
      startStuck = true;
      logEvent("START held %lu s: treated as stuck, nothing started", held / 1000UL);
      displayShowNotice(TRS("START BUTTON", "BOTON START"), TRS("Stuck|Check button", "Atascado|Revise el boton"),
                        START_STUCK_MS + 1000);
    } else if (held >= START_HOLD_MS && !startLong) {
      startLong = true;
      beep(BEEP_TICK);
      showStartChoice(true);
    }
    return;
  }
  if (!startDown) return;
  startDown = false;                    // just released
  if (activeState != stIdle) return;
  if (startStuck) {
    displayClearNotice();
    return;
  }
  inStart = true;                       // one pass, like the app's Start
  startNoSave = startLong;
}

void updateInputs() {
  readStartButton();
  inStop  = debounceButton(STOP_BUTTON,  lastStopButtonPressTime,  lastStopButtonState);

  // A float-switch disagreement is reported once, and again only after it cleared.
  // (The switches themselves are read and debounced by the safety task.)
  bool fault = tankFault();
  if (fault && !tankFaultReported) {
    tankFaultReported = true;
    logEvent("Level switches disagree: high wet, low dry");
    beep(BEEP_ALARM);
    notify(TR("Reuse Water: check level switches", "Reúso de Agua: revise los flotadores"),
           TR("The high-level switch reports water but the low-level switch does not. "
              "The tank is treated as full and the pump is not used until the switches agree.",
              "El flotador alto indica agua, pero el bajo no. El tanque se trata como lleno "
              "y la bomba no se usa hasta que ambos flotadores coincidan."), 4, "warning");
  } else if (!fault && tankFaultReported) {
    tankFaultReported = false;
    logEvent("Level switches agree again");
  }
}

// ── Relay presets ──────────────────────────────────────────────────────────
// The cycle only requests a mode; GreyWaterOutputs applies it, with the safety
// interlocks (see GreyWaterOutputs.h for the pin table of each mode).
void fillWashingMachineWithTapWater()  { DEBUG_PRINT("Filling with tap water.");               outputsRequest(modeTapFill); }
void fillWashingMachineWithTankWater() { DEBUG_PRINT("Filling with tank water + washing tank."); outputsRequest(modeTankFill); }
void drainSoapyWater()                 { DEBUG_PRINT("Draining soapy water.");                 outputsRequest(modeDrainSoapy); }
void noSavingWater()                   { DEBUG_PRINT("No saving water.");                      outputsRequest(modeStandby); }

// ── Water level ────────────────────────────────────────────────────────────
WaterLevel tankLevel() {
  switch (tankLevelRaw()) {
    case TankLevel::High:   return high;
    case TankLevel::Medium: return medium;
    default:                return low;
  }
}
WaterLevel getWaterLevelTank(int, int) { return tankLevel(); }

// ── Fill / drain logic ─────────────────────────────────────────────────────
// Where the fill water comes from, for the screen (set by checkAndFillWashingMachine).
enum FillSource { fillFromTap, fillFromTank, fillTankLow };
static FillSource fillSource = fillFromTap;

const char* checkAndFillWashingMachine() {
  if (!tankHasWater()) {           // low float dry (also when the switches disagree): tap water
    pumpAllow(false);
    fillWashingMachineWithTapWater();
    fillSource = fillTankLow;
    return TR("Tank low: using tap water", "Tanque bajo: usando agua de la llave");
  }

  // The machine is taking water: supply it from the tank, unless PumpGuard
  // stopped the pump during this draw (it waits for the next draw).
  bool wantTank = gates.tapSeen && tapFlow().flowing();
  if (pumpAllow(wantTank)) {
    if (outputsMode() != modeTankFill) logEvent("Pump on: reusing tank water");
    fillWashingMachineWithTankWater();
    fillSource = fillFromTank;
    return TR("Using tank water", "Usando agua del tanque");
  }
  fillWashingMachineWithTapWater();
  fillSource = fillFromTap;
  return TR("Using tap water", "Usando agua de la llave");
}

// Step 4: rinse water goes to the tank until the high float reports water.
// Once full, saving stays off for the rest of the step, so a float bobbing at
// the high mark cannot switch the fill valve on and off.
//
// From 3.4 the tank only takes water while the drain flow is above
// cfg.saveMinLpm (Settings: Minimum flow to save). At or below it, the end of
// a pump-out trickling, the fill valve closes and the drain opens on the same
// pass. If the machine pumps again (a spin), saving resumes once the flow has
// stayed above the limit for SAVE_RESUME_MS, so the valves do not chatter.
// "Reuse only" cycle (app button, 3.7): step 1 reuses tank water as usual, but
// in step 4 the rinse water goes to the drain; the fill valve stays closed.
static bool cycleNoSave = false;       // set when the cycle starts, for the whole cycle
static bool saveStoppedFull = false;   // reset when step 4 begins
static bool saveLowFlow     = false;   // drain flow at or below cfg.saveMinLpm: draining
static bool saveAbove       = false;   // ... flow back above the limit since saveAboveMs
static unsigned long saveAboveMs = 0;

static void resetSaveFlow() { saveLowFlow = false; saveAbove = false; }

const char* checkAndFillWaterTank() {
  if (cycleNoSave) {
    noSavingWater();                   // fill valve closed, drain open
    return TR("Reuse only: the rinse water goes to the drain", "Solo reusar: el agua de enjuague va al desagüe");
  }
  if (tankFull()) saveStoppedFull = true;
  DEBUG_PRINT(saveStoppedFull ? "Tank full." : "Tank has room.");

  float lpm = drainFlow().rateLpm();
  if (!saveStoppedFull && !saveLowFlow && lpm <= cfg.saveMinLpm) {
    saveLowFlow = true;
    saveAbove = false;
    logEvent("Saving paused: drain flow %.1f L/min (limit %.1f), to the drain", lpm, cfg.saveMinLpm);
  } else if (!saveStoppedFull && saveLowFlow) {
    if (lpm <= cfg.saveMinLpm) {
      saveAbove = false;
    } else if (!saveAbove) {
      saveAbove = true;
      saveAboveMs = millis();
    } else if (millis() - saveAboveMs >= SAVE_RESUME_MS) {
      saveLowFlow = false;
      logEvent("Saving resumed: drain flow %.1f L/min", lpm);
    }
  }

  if (saveStoppedFull) {
    noSavingWater();
    return TR("Tank full: not saving", "Tanque lleno: no se guarda agua");
  }
  if (saveLowFlow) {
    noSavingWater();                   // fill valve closed, drain open
    return TR("Low flow: to the drain", "Flujo bajo: va al desagüe");
  }
  outputsRequest(modeSaveTank);        // the safety task closes it at once if the tank fills
  return TR("Saving water to tank", "Guardando agua en el tanque");
}

// ── Flow rate readings ─────────────────────────────────────────────────────
// sampleFlowSensors() runs on every loop() pass, in every state, so the meters
// always know whether water is flowing. Measured litres are added to this
// cycle's totals according to what the outputs are actually doing.
static float cycleTapL    = 0.0f;   // tap water into the machine this cycle
static float cycleReusedL = 0.0f;   // tank water into the machine this cycle
static float cycleSavedL  = 0.0f;   // rinse water into the tank this cycle

// What the safety task did since the last pass: log it and raise alerts here,
// because the event log and alerts belong to loop().
static void handleSafetyEvents() {
  SafetyEvents e;
  if (!safetyTakeEvents(e)) return;
  if (e.pumpStopped) {
    logEvent(e.pumpStoppedDry ? "Pump stopped: no water after starting" : "Pump stopped: machine closed its inlet");
    if (e.pumpStoppedDry && e.dryStarts >= PUMP_DRY_ALERT && !pumpDryAlertSent) {
      pumpDryAlertSent = true;
      beep(BEEP_ALARM);
      notify(TR("Reuse Water: tank pump not pumping", "Reúso de Agua: la bomba no bombea"),
             TR("The tank pump started several times without any water flow, so tap water was used. "
                "Check the pump, its intake and the hose for an air lock.",
                "La bomba del tanque arrancó varias veces sin flujo de agua, así que se usó agua de la llave. "
                "Revise la bomba, su entrada y la manguera por si hay aire."), 4, "warning");
    }
  }
  if (e.washClosedFull) logEvent("Safety: tank full, wash valve closed");
}

void sampleFlowSensors() {
  buzzerUpdate();
  RelayMode mode = outputsMode();
  float tapL, drainL;
  flowSample(tapL, drainL);
  pumpNoteFlowStarts(tapFlow().starts());

  if (mode == modeTankFill)      cycleReusedL += tapL;
  else if (mode == modeTapFill)  cycleTapL    += tapL;
  if (mode == modeSaveTank)      cycleSavedL  += drainL;

  handleSafetyEvents();
}

// ── Sensor reset ───────────────────────────────────────────────────────────
// Called when a step begins: the step will look only for flow that starts
// (or, for step 1, is present) from now on. Nothing is left latched.
void resetFlowSensors() {
  gates.enter(tapFlow(), drainFlow());
  pumpResetForStep();
  DEBUG_PRINT("Flow detection re-armed for the new step.");
}

// ── State entry handlers ───────────────────────────────────────────────────
static bool manualWashActive = false;      // manual tank wash running (Idle only)
static unsigned long manualWashStart = 0;
static const char* manualWashResult = "";  // how the last manual wash ended (dashboard)
static bool cycleStarted = false;          // true once Start has been pressed since boot
static bool cycleActive  = false;          // a cycle is running (Run .. Drain2)
static unsigned long drain2StartTime = 0;  // for the Drain2 countdown on the OLED
static unsigned long stepStartTime   = 0;  // entry time of the current step 1-4
static bool stepAlertSent     = false;     // "check sensors" alert sent for this step
static bool tankFullAlertSent = false;     // "tank full" alert sent this cycle

int  activeState = stIdle;
bool drain2Done  = false;

// Adds this cycle's measured water to the monthly and all-time totals.
static unsigned long cycleStartMillis = 0;  // when Start was pressed (for the duration)
static unsigned long lastEndMillis    = 0;  // when the last wash ended, since boot (0 = not since boot)

static void recordCycleStats(bool completed) {
  statsRollMonth();
  stats.lastEndEpoch  = timeIsValid() ? (uint32_t)time(nullptr) : 0;
  stats.lastDurationS = (millis() - cycleStartMillis) / 1000UL;
  stats.lastTapL      = cycleTapL;
  stats.lastReusedL   = cycleReusedL;
  stats.lastSavedL    = cycleSavedL;
  stats.lastCompleted = completed ? 1 : 0;
  stats.lastValid     = 1;
  lastEndMillis       = millis();
  stats.totalTapL    += cycleTapL;     stats.monthTapL    += cycleTapL;
  stats.totalReusedL += cycleReusedL;  stats.monthReusedL += cycleReusedL;
  stats.totalSavedL  += cycleSavedL;   stats.monthSavedL  += cycleSavedL;
  if (completed) { stats.totalCycles++; stats.monthCycles++; }
  statsSave();
}

// Writes what the running cycle has done so far to flash (3.10), so a cycle
// cut short can be resumed from the app. Called when a cycle starts, at each
// step and when the machine has filled in step 1 or 3: a few writes per cycle.
static void saveCycleSnapshot() {
  CycleSnapshot s;
  memset(&s, 0, sizeof(s));
  s.state   = (uint8_t)activeState;
  s.noSave  = cycleNoSave ? 1 : 0;
  s.filled  = (activeState == stFill1 || activeState == stFill2) && gates.filled ? 1 : 0;
  s.epoch   = timeIsValid() ? (uint32_t)time(nullptr) : 0;
  s.cycleS  = (millis() - cycleStartMillis) / 1000UL;
  s.tapL    = cycleTapL;
  s.reusedL = cycleReusedL;
  s.savedL  = cycleSavedL;
  cycleSnapSave(s);
}

void handleIdleStateEntry() {
  DEBUG_PRINT("Idle.");

  // inStop is still true here only if the user just pressed Stop; a cycle
  // that finished naturally (Drain2 -> Idle timeout) will have inStop false.
  bool stoppedByUser = inStop;

  // Button states
  inStart = false;
  inStop  = false;

  // Cycle-result flags: these drive the Fill1->Drain1->Fill2->Drain2 transitions.
  // If they're not cleared here, a "true" left over from the previous cycle can
  // make the FSM skip straight past a state the next time Start is pressed,
  // before that state's own logic gets a chance to recompute it.
  filled1  = false;
  drained1 = false;
  filled2  = false;
  drain2Done = false;
  manualWashActive = false;

  resetFlowSensors();
  resetTankWash();

  // lastCheckTime is shared across every state's periodic logic. Reset it here
  // too, otherwise the first fill/drain check after Start can fire on the very
  // first loop() pass using a timer left over from the previous cycle.
  lastCheckTime = millis();

  // Only show a result notice after a real cycle; at boot onEnter() also lands
  // here with inStop false, which used to show "Cycle Complete!" on power-up.
  if (cycleStarted) {
    if (stoppedByUser) {
      displayShowNotice(TRS("STOPPED", "DETENIDO"), TRS("Cycle cancelled", "Ciclo cancelado"), NOTICE_DURATION);
    } else {                                    // tell the household what the cycle saved
      char done[64] = "", part[24];             // the header already says DONE: only the litres
      if (cycleReusedL >= 1.0f) {
        snprintf(part, sizeof(part), TRS("Reused %.0f L", "Reusados %.0f L"), cycleReusedL);
        strlcat(done, part, sizeof(done));
      }
      if (cycleSavedL >= 1.0f) {
        snprintf(part, sizeof(part), TRS("Saved %.0f L", "Guardados %.0f L"), cycleSavedL);
        if (done[0]) strlcat(done, "|", sizeof(done));
        strlcat(done, part, sizeof(done));
      }
      if (!done[0]) strlcpy(done, TRS("Cycle complete", "Ciclo completo"), sizeof(done));
      displayShowNotice(TRS("DONE", "TERMINADO"), done, NOTICE_DURATION);
    }
  }

  if (cycleActive) {
    cycleActive = false;
    recordCycleStats(!stoppedByUser);
    char body[320];
    const char* how = stoppedByUser ? TR("The cycle was stopped.", "Se detuvo el ciclo.")
                                    : TR("The wash cycle is complete.", "El ciclo de lavado terminó.");
    if (cycleNoSave)
      snprintf(body, sizeof(body),
               TR("%s Tank water reused: %.0f L. Reuse only: the rinse water went to the drain.",
                  "%s Agua del tanque reusada: %.0f L. Solo reusar: el agua de enjuague fue al desagüe."),
               how, cycleReusedL);
    else
      snprintf(body, sizeof(body),
               TR("%s Tank water reused: %.0f L. Rinse water saved: %.0f L.",
                  "%s Agua del tanque reusada: %.0f L. Agua de enjuague guardada: %.0f L."),
               how, cycleReusedL, cycleSavedL);
    // Routine news is low priority (no sound on most phones); problems stay high.
    notify(stoppedByUser ? TR("Reuse Water: cycle stopped", "Reúso de Agua: ciclo detenido")
                         : TR("Reuse Water: cycle complete", "Reúso de Agua: ciclo completo"),
           body, stoppedByUser ? 1 : 2, stoppedByUser ? "stop_sign" : "white_check_mark");
    logEvent(stoppedByUser ? "Cycle stopped" : "Cycle complete: reused %.0f L, saved %.0f L", cycleReusedL, cycleSavedL);
    if (!stoppedByUser) beep(BEEP_DONE);
    cycleMarkEnd();          // only a cycle that really ran clears the marker; at power-up bootChecks() reads it first
  }
  cycleNoSave = false;
  stepStartTime = 0;
  DEBUG_PRINT("Idle state fully reset. Ready for next Start.");
}

void handleRunStateEntry() {
  DEBUG_PRINT("Run.");
  cycleStarted = true;
  cycleActive  = true;
  cycleStartMillis = millis();
  interruptedFlag = false;   // a new cycle replaces the "interrupted" notice
  cycleNoSave = startNoSave;   // "Reuse only" from the app, or START held 2 s on the panel
  startNoSave = false;
  logEvent(cycleNoSave ? "Cycle started: reuse only, rinse water to the drain" : "Cycle started");
  if (manualWashActive) {    // starting a cycle ends a manual wash
    manualWashActive = false;
    manualWashResult = "cycle";
  }
  cycleTapL = cycleReusedL = cycleSavedL = 0.0f;
  pumpResetForCycle();
  pumpDryAlertSent = false;
  tankFullAlertSent = false;
  saveCycleSnapshot();
  displayClearNotice();  // Start pressed: drop "Cycle stopped/complete" right away
}

static const char* const STEP_EVENT[STATE_COUNT] = { "", "", "Step 1/4 wash started", "Step 2/4 soapy drain started",
                                                    "Step 3/4 rinse started", "Step 4/4 save started" };
static bool resuming         = false;   // the next step entry is a Resume from the app (3.10)
static bool resumeFilledStep = false;   // ... and the machine had already filled in that step
static bool stepTimed        = false;   // stepStartTime is the real start of the step (not a resume)

static void handleFillDrainEntry(const char* label) {
  DEBUG_PRINT(label);
  bool resumed = resuming;
  resuming = false;
  if (resumed) { /* takeResumeRequest() logged it */ }
  else if (activeState == stDrain2 && cycleNoSave) logEvent("Step 4/4 final drain started (reuse only, not saving)");
  else if (activeState >= stFill1 && activeState < STATE_COUNT) logEvent("%s", STEP_EVENT[activeState]);
  resetFlowSensors();
  if (resumed && resumeFilledStep) gates.resumeFilled(drainFlow());   // wait only for the drain that ends the step
  resumeFilledStep = false;
  resetTankWash();
  stepStartTime = millis();
  stepTimed = !resumed;      // a resumed step is not used for the typical step times
  stepAlertSent = false;
  saveCycleSnapshot();
}

// Typical step times: a running average of how long steps 1-3 take on this
// machine, so the app can say "usually about 25 min".
static void recordStepDuration(int step) {
  if (stepStartTime == 0 || !stepTimed || step < 0 || step > 2) return;
  uint32_t s = (millis() - stepStartTime) / 1000UL;
  if (stats.stepSamples[step] == 0) stats.stepAvgS[step] = s;
  else stats.stepAvgS[step] = (stats.stepAvgS[step] * 3 + s) / 4;
  if (stats.stepSamples[step] < 65535) stats.stepSamples[step]++;
}

void handleFill1StateEntry()  { handleFillDrainEntry("1st Fill.");  }
void handleDrain1StateEntry() { recordStepDuration(0); handleFillDrainEntry("1st Drain."); }
void handleFill2StateEntry()  { recordStepDuration(1); handleFillDrainEntry("2nd Fill.");  }
void handleDrain2StateEntry() {
  recordStepDuration(2);
  handleFillDrainEntry("2nd Drain.");
  drain2StartTime = millis();
  drain2Done = false;
  saveStoppedFull = false;
  resetSaveFlow();
}

// ── State logic ────────────────────────────────────────────────────────────
// Steps 1-3 end only when the flow sensors see the expected water. If that
// never happens the step waits; after cfg.stepAlertMs an alert is sent once and
// the OLED asks the user to check the sensors. The cycle itself keeps waiting,
// exactly as before, so a slow washing machine is never cut short.
static bool stepIsStuck() {
  if (stepStartTime == 0 || millis() - stepStartTime < cfg.stepAlertMs) return false;
  if (!stepAlertSent) {
    stepAlertSent = true;
    char body[320];
    snprintf(body, sizeof(body),
             TR("Step %d of 4 has run for %lu minutes without the expected water flow. "
                "Check the flow sensors and that the washing machine is running.",
                "El paso %d de 4 lleva %lu minutos sin el flujo de agua esperado. "
                "Revise los sensores de flujo y que la lavadora esté funcionando."),
             activeState - stFill1 + 1, (unsigned long)(cfg.stepAlertMs / 60000UL));
    beep(BEEP_ALARM);
    notify(TR("Reuse Water: check sensors", "Reúso de Agua: revise los sensores"), body, 4, "warning");
  }
  return true;
}

// Screen (ASCII) and app texts when a step waits too long for water. The
// screen says what to do, as the Quick Card does.
#define STUCK_SHORT   TRS("No water flow|Check washer|Call installer", "Sin flujo|Revise lavadora|Pida ayuda")
#define STUCK_MESSAGE TR("No flow seen: check sensors", "No se detecta flujo: revise los sensores")

static void periodicAction(unsigned long& lastTime, void(*action)(), const char* debugMsg,
                           const char* title, const char* message, const char* detail) {
  if (millis() - lastTime >= OPERATION_DELAY) {
    action();
    DEBUG_PRINT(debugMsg);
    displaySetStatus(title, message, detail);
    lastTime = millis();
  }
}

// ── Manual tank wash ───────────────────────────────────────────────────────
// Started from the dashboard while the system is Idle and the tank is not
// full. The tank-wash valve opens and stays open until the high-level switch
// reports water, which means the tank is full. The other outputs stay as in
// Idle (tap and drain on, fill valve and pump off).
//
// The tank is treated as full as soon as the high-level switch reads HIGH,
// even if the low-level switch disagrees: closing early is safe, overflowing
// is not. A disagreement is reported as a sensor problem.
//
// The wash also ends when STOP is pressed (controller or dashboard), when a
// cycle starts, or after cfg.manualWashMs as a safety limit in case a level
// switch has failed.
static bool tankHighSwitchWet() { return digitalRead(IN_HIGHLEVEL) == HIGH; }
static bool tankLowSwitchWet()  { return digitalRead(IN_LOWLEVEL)  == HIGH; }

ManualWashStart startManualWash() {
  if (activeState != stIdle) return washBusy;
  if (tankHighSwitchWet())   return washTankFull;
  manualWashActive = true;
  manualWashStart  = millis();
  manualWashResult = "";
  displayClearNotice();
  lastCheckTime = millis() - OPERATION_DELAY;   // open the valve on the next pass
  logEvent("Manual tank wash started");
  return washStarted;
}

// Ends the wash, closes the valve at once and tells the user why.
static void endManualWash(const char* result) {
  if (!manualWashActive) return;
  manualWashActive = false;
  manualWashResult = result;
  noSavingWater();                              // Idle outputs: wash valve closed
  lastCheckTime = millis();
  logEvent("Manual tank wash ended: %s", result);

  if (strcmp(result, "full") == 0) {
    if (tankLowSwitchWet()) {
      displayShowNotice(TRS("TANK CLEANED", "TANQUE LIMPIO"), TRS("Tank full", "Tanque lleno"), NOTICE_DURATION);
      notify(TR("Reuse Water: tank wash complete", "Reúso de Agua: lavado del tanque terminado"),
             TR("The tank is full and the wash valve is closed.",
                "El tanque está lleno y la válvula de lavado está cerrada."), 2, "droplet");
    } else {
      displayShowNotice(TRS("WASH STOPPED", "LAVADO DETENIDO"), TRS("Check tank|Call installer", "Revise tanque|Pida ayuda"), NOTICE_DURATION);
      beep(BEEP_ALARM);
      notify(TR("Reuse Water: check level switches", "Reúso de Agua: revise los flotadores"),
             TR("The tank wash stopped because the high-level switch reports water, but the "
                "low-level switch does not. Check both float switches.",
                "El lavado del tanque se detuvo porque el flotador alto indica agua, pero el "
                "bajo no. Revise ambos flotadores."), 4, "warning");
    }
  } else if (strcmp(result, "timeout") == 0) {
    char body[320];
    snprintf(body, sizeof(body),
             TR("The tank did not report full within %lu minutes, so the wash valve was closed. "
                "Check the high-level switch and the wash water supply.",
                "El tanque no indicó lleno en %lu minutos, así que se cerró la válvula de lavado. "
                "Revise el flotador alto y el suministro de agua de lavado."),
             (unsigned long)(cfg.manualWashMs / 60000UL));
    displayShowNotice(TRS("WASH STOPPED", "LAVADO DETENIDO"), TRS("Check tank|Call installer", "Revise tanque|Pida ayuda"), NOTICE_DURATION);
    beep(BEEP_ALARM);
    notify(TR("Reuse Water: tank wash time limit", "Reúso de Agua: límite de tiempo del lavado"), body, 4, "warning");
  } else {
    displayShowNotice(TRS("WASH STOPPED", "LAVADO DETENIDO"), TRS("Tank not full", "Tanque no lleno"), NOTICE_DURATION);
  }
}

void stopManualWash() {
  endManualWash("stopped");
}

void idleStateLogic() {
  if (!manualWashActive) {
    // What to do; the tank icon in the corner shows how full the tank is.
    periodicAction(lastCheckTime, noSavingWater, "System inactive.", TRS("READY", "LISTO"),
                   TRS("Start washer|Press START", "Inicie lavadora|y pulse START"), TR("Press START", "Pulse START"));
    return;
  }

  // These checks run on every loop() pass, not every 250 ms, so the valve
  // closes the moment the tank is full.
  if (inStop)              { endManualWash("stopped"); return; }   // STOP button
  if (tankHighSwitchWet()) { endManualWash("full");    return; }
  unsigned long elapsed = millis() - manualWashStart;
  if (elapsed >= cfg.manualWashMs) { endManualWash("timeout"); return; }

  if (millis() - lastCheckTime < OPERATION_DELAY) return;
  lastCheckTime = millis();

  outputsRequest(modeManualWash);   // as Idle, plus the wash valve (the safety task closes it when full)
  unsigned long left = (cfg.manualWashMs - elapsed) / 1000UL;
  char msg[64];
  snprintf(msg, sizeof(msg), TRS("Until full|%lu:%02lu left", "Hasta llenarse|Quedan %lu:%02lu"),
           left / 60UL, left % 60UL);
  displaySetStatus(TRS("TANK CLEANING", "LIMPIEZA TANQUE"), msg, TR("Washing until the tank is full", "Lavando hasta que el tanque se llene"));
}

void runStateLogic() {
  periodicAction(lastCheckTime, noSavingWater, "System running.", TRS("STARTING", "INICIANDO"),
                 cycleNoSave ? TRS("Getting ready...|Reuse only", "Preparando...|Solo reusar") : TRS("Getting ready...", "Preparando..."),
                 cycleNoSave ? TR("Starting: reuse only, the rinse water goes to the drain", "Iniciando: solo reusar, el enjuague va al desagüe")
                             : TR("Starting...", "Iniciando..."));
}

// About how many minutes the cycle still needs, from this machine's typical
// step times (steps 1-3) and the fixed length of step 4. -1 until every step
// has been timed at least once.
static long cycleMinutesLeft() {
  if (activeState < stFill1 || activeState > stFill2 || stepStartTime == 0) return -1;
  for (int i = 0; i < 3; ++i) if (stats.stepSamples[i] == 0) return -1;
  int cur = activeState - stFill1;
  uint32_t elapsed = (millis() - stepStartTime) / 1000UL;
  uint32_t left = stats.stepAvgS[cur] > elapsed + 60 ? stats.stepAvgS[cur] - elapsed : 60;   // an overdue step: at least 1 min
  for (int i = cur + 1; i < 3; ++i) left += stats.stepAvgS[i];
  left += cfg.drain2DurationMs / 1000UL;
  return (long)((left + 59) / 60);
}

// Adds the "25 min left" page to a step's screen message, when it is known.
static const char* withTimeLeft(const char* pages) {
  static char buf[96];
  long min = cycleMinutesLeft();
  if (min < 0) return pages;
  snprintf(buf, sizeof(buf), TRS("%s|%lu min left", "%s|Faltan %lu min"), pages, (unsigned long)min);
  return buf;
}

void fill1StateLogic() {
  if (millis() - lastCheckTime >= OPERATION_DELAY) {
    bool wasFilled = gates.filled;
    filled1 = gates.fill1Done(tapFlow(), drainFlow(), cfg.minFillLitres);   // filled, then a drain started
    if (gates.filled && !wasFilled) {
      logEvent("Machine filled (%.1f L); washing", gates.litresTaken(tapFlow()));
      saveCycleSnapshot();   // a resume after a cut now waits only for the drain
    }
    const char* fillMsg = checkAndFillWashingMachine();
    DEBUG_PRINT(fillMsg);
    // Screen text for the household: what the machine and the grey water system are doing.
    const char* fillShort = fillSource == fillFromTank ? TRS("Reusing water", "Reusando agua")
                          : fillSource == fillFromTap  ? TRS("Using tap water", "Agua de la llave")
                                                       : TRS("Tank low|Using tap water", "Tanque bajo|Agua de la llave");
    const char* step1 = TRS("WASH 1 OF 4", "LAVADO 1 DE 4");
    if (stepIsStuck())        displaySetStatus(step1, STUCK_SHORT, STUCK_MESSAGE);
    else if (gates.filled)    displaySetStatus(step1, withTimeLeft(TRS("Washing...", "Lavando...")), TR("The machine is washing", "La lavadora está lavando"));
    else if (!gates.tapSeen)  displaySetStatus(step1, withTimeLeft(TRS("Waiting to fill", "Esperando agua")),
                                               TR("Waiting for the machine to take water", "Esperando a que la lavadora tome agua"));
    else                      displaySetStatus(step1, withTimeLeft(fillShort), fillMsg);
    lastCheckTime = millis();
  }
}

void drain1StateLogic() {
  if (millis() - lastCheckTime >= OPERATION_DELAY) {
    drainSoapyWater();
    drained1 = gates.drain1Done(tapFlow());               // a new fill (the rinse) started
    DEBUG_PRINT("1st drain: draining soapy water.");
    const char* step2 = TRS("DRAIN 2 OF 4", "DESAGUE 2 DE 4");
    if (stepIsStuck()) displaySetStatus(step2, STUCK_SHORT, STUCK_MESSAGE);
    else               displaySetStatus(step2, withTimeLeft(TRS("Draining|soapy water", "Desaguando|agua jabonosa")),
                                        TR("Draining soapy water", "Desaguando agua con jabón"));
    lastCheckTime = millis();
  }
}

void fill2StateLogic() {
  if (millis() - lastCheckTime >= OPERATION_DELAY) {
    fillWashingMachineWithTapWater();
    bool wasFilled = gates.filled;
    filled2 = gates.fill2Done(tapFlow(), drainFlow(), cfg.minFillLitres);   // rinse filled, then a new drain started
    if (gates.filled && !wasFilled) {
      logEvent("Machine filled (%.1f L); rinsing", gates.litresTaken(tapFlow()));
      saveCycleSnapshot();
    }
    DEBUG_PRINT("2nd fill: rinsing with tap water.");
    const char* step3 = TRS("RINSE 3 OF 4", "ENJUAGUE 3 DE 4");
    if (stepIsStuck())     displaySetStatus(step3, STUCK_SHORT, STUCK_MESSAGE);
    else if (gates.filled) displaySetStatus(step3, withTimeLeft(TRS("Rinsing...", "Enjuagando...")), TR("The machine is rinsing", "La lavadora está enjuagando"));
    else                   displaySetStatus(step3, withTimeLeft(TRS("Using tap water", "Agua de la llave")),
                                            TR("Rinsing with tap water", "Enjuagando con agua de la llave"));
    lastCheckTime = millis();
  }
}

void drain2StateLogic() {
  if (millis() - lastCheckTime >= OPERATION_DELAY) {
    const char* tankMsg = checkAndFillWaterTank();
    DEBUG_PRINT(tankMsg);

    if (!cycleNoSave && (saveStoppedFull || tankFull()) && !tankFullAlertSent) {   // not when only the flow is low
      tankFullAlertSent = true;
      notify(TR("Reuse Water: tank full", "Reúso de Agua: tanque lleno"),
             TR("The storage tank is full. The rest of the rinse water is going to the drain.",
                "El tanque de almacenamiento está lleno. El resto del agua de enjuague va al desagüe."),
             2, "droplet");
    }

    // The last page shows the time left until the cycle ends.
    unsigned long elapsed = millis() - drain2StartTime;
    drain2Done = (elapsed >= cfg.drain2DurationMs);
    unsigned long left = drain2Done ? 0 : (cfg.drain2DurationMs - elapsed) / 1000UL;
    char msg[64];
    if (cycleNoSave) {
      snprintf(msg, sizeof(msg), TRS("Not saving|To the drain|Ends in %lu:%02lu", "Sin guardar|Al desague|Fin en %lu:%02lu"),
               left / 60UL, left % 60UL);
      displaySetStatus(TRS("DRAIN 4 OF 4", "DESAGUE 4 DE 4"), msg, tankMsg);
      lastCheckTime = millis();
      return;
    }
    if (outputsMode() == modeSaveTank)
      snprintf(msg, sizeof(msg), TRS("Filling tank|Ends in %lu:%02lu", "Agua al tanque|Fin en %lu:%02lu"),
               left / 60UL, left % 60UL);
    else if (saveLowFlow && !saveStoppedFull && !tankFull())
      snprintf(msg, sizeof(msg), TRS("Low flow|To the drain|Ends in %lu:%02lu", "Flujo bajo|Al desague|Fin en %lu:%02lu"),
               left / 60UL, left % 60UL);
    else
      snprintf(msg, sizeof(msg), TRS("Tank full|Ends in %lu:%02lu", "Tanque lleno|Fin en %lu:%02lu"),
               left / 60UL, left % 60UL);
    displaySetStatus(TRS("SAVE 4 OF 4", "AHORRO 4 DE 4"), msg, tankMsg);
    lastCheckTime = millis();
  }
}

// ── Remote control (web dashboard) ─────────────────────────────────────────
// The web server runs inside loop(), so these flags are only ever touched from
// one task. applyRemoteRequests() turns them into a one-pass button press.
static bool remoteStopRequested  = false;
static bool remoteStartRequested = false;

void requestRemoteStop() {
  remoteStopRequested = true;
}

static bool remoteStartNoSave    = false;   // the requested start is "Reuse only"

bool requestRemoteStart(bool save) {
  if (!cfg.allowRemoteStart || activeState != stIdle) return false;
  remoteStartRequested = true;
  remoteStartNoSave    = !save;
  return true;
}

// ── Resume a cycle cut short (3.10) ────────────────────────────────────────
// At power-up the controller always starts in Ready with every output off.
// If flash held a complete snapshot, the app may ask to resume: the cycle
// continues at the start of the saved step, with its mode ("Reuse only") and
// the water counted so far. A step 1 or 3 in which the machine had already
// filled waits only for the drain that ends it. Offered for RESUME_OFFER_MS
// after power-up, while Ready and no tank wash runs, and not when the saved
// state is more than RESUME_MAX_AGE_S old (only known once the clock is set).
static bool remoteResumeRequested = false;

bool resumeAvailable() {
  if (!interruptedFlag || !interruptedResumable) return false;
  if (activeState != stIdle || manualWashActive) return false;
  if (millis() - interruptedAtMs >= RESUME_OFFER_MS) return false;
  if (interruptedSnap.epoch != 0 && timeIsValid() &&
      (uint32_t)time(nullptr) - interruptedSnap.epoch > RESUME_MAX_AGE_S) return false;
  return true;
}

uint32_t resumeLeftS() {
  if (!resumeAvailable()) return 0;
  return (RESUME_OFFER_MS - (millis() - interruptedAtMs)) / 1000UL;
}

bool requestResume() {
  if (!resumeAvailable()) return false;
  remoteResumeRequested = true;
  return true;
}

bool takeResumeRequest(int& state) {
  if (!remoteResumeRequested) return false;
  remoteResumeRequested = false;
  if (!resumeAvailable() || inStart) return false;   // a START on the panel in the same pass wins
  const CycleSnapshot& s = interruptedSnap;
  state = s.state < stFill1 ? stFill1 : s.state;     // cut while starting: begin at step 1
  // What Start does (handleRunStateEntry), with the saved cycle.
  cycleStarted = true;
  cycleActive  = true;
  cycleStartMillis = millis() - s.cycleS * 1000UL;   // the duration leaves out the time without power
  interruptedFlag = false;
  cycleNoSave = s.noSave != 0;
  startNoSave = false;
  cycleTapL = s.tapL; cycleReusedL = s.reusedL; cycleSavedL = s.savedL;
  pumpResetForCycle();
  pumpDryAlertSent = false;
  tankFullAlertSent = false;
  resuming = true;
  resumeFilledStep = s.filled && (state == stFill1 || state == stFill2);
  logEvent("Cycle resumed: step %d/4%s%s", state - 1, cycleNoSave ? ", reuse only" : "",
           resumeFilledStep ? ", was filled" : "");
  displayShowNotice(TRS("RESUMED", "REANUDADO"), TRS("Cycle resumed", "Ciclo reanudado"), NOTICE_DURATION);
  return true;
}

void applyRemoteRequests() {
  if (remoteStopRequested) {
    remoteStopRequested = false;
    if (activeState != stIdle) inStop = true;
  }
  if (remoteStartRequested) {
    remoteStartRequested = false;
    if (activeState == stIdle) { inStart = true; startNoSave = remoteStartNoSave; }
  }
}

// ── Tank water age ─────────────────────────────────────────────────────────
// The clock starts when the tank goes from empty (below the low switch) to
// holding water, and stops when it is empty again. That measures the age of
// the oldest water in the tank, which is what matters for odour and hygiene.
// The start time is stored in flash once the clock is set by NTP; before
// that, a millis() fallback is used until the next power cut.
static unsigned long tankSinceMillis   = 0;
static bool          tankAgeAlertSent  = false;

static float tankAgeHours() {
  if (stats.tankSinceEpoch != 0 && timeIsValid()) {
    return (float)(time(nullptr) - (time_t)stats.tankSinceEpoch) / 3600.0f;
  }
  if (tankSinceMillis != 0) {
    return (float)(millis() - tankSinceMillis) / 3600000.0f;
  }
  return -1.0f;
}

void systemPeriodic() {
  static unsigned long lastCheck = 0;
  static WaterLevel    lastLevel = low;
  static uint8_t       sameCount = 0;
  if (millis() - lastCheck < TANK_CHECK_PERIOD) return;
  lastCheck = millis();

  if (statsRollMonth()) statsSave();   // a new month starts even without a wash

  // Require three equal readings in a row (30 s) so sloshing does not reset the age.
  WaterLevel level = tankLevel();
  if (level == lastLevel) { if (sameCount < 3) sameCount++; }
  else                    { lastLevel = level; sameCount = 1; }
  if (sameCount < 3) return;

  bool known = (stats.tankSinceEpoch != 0) || (tankSinceMillis != 0);
  if (level == low) {
    if (known) {                         // tank emptied: reset the age
      stats.tankSinceEpoch = 0;
      tankSinceMillis      = 0;
      tankAgeAlertSent     = false;
      statsSave();
    }
  } else if (!known) {                   // water arrived in an empty tank
    tankSinceMillis = millis();
    if (timeIsValid()) { stats.tankSinceEpoch = (uint32_t)time(nullptr); statsSave(); }
  } else if (stats.tankSinceEpoch == 0 && tankSinceMillis != 0 && timeIsValid()) {
    // Clock became valid after the age started: convert the fallback to a real time.
    stats.tankSinceEpoch = (uint32_t)(time(nullptr) - (millis() - tankSinceMillis) / 1000UL);
    statsSave();
  }

  float age = tankAgeHours();
  if (!tankAgeAlertSent && age >= cfg.tankMaxAgeHours) {
    tankAgeAlertSent = true;
    char body[320];
    snprintf(body, sizeof(body),
             TR("The water in the tank is %.0f hours old. Use it on the next wash, "
                "or empty and clean the tank.",
                "El agua del tanque tiene %.0f horas. Úsela en el próximo lavado, "
                "o vacíe y limpie el tanque."), age);
    notify(TR("Reuse Water: tank water is old", "Reúso de Agua: el agua del tanque es vieja"), body, 3, "hourglass");
  }
}

// ── Status snapshot for the dashboard ──────────────────────────────────────
static const char* const STATE_LABELS[STATE_COUNT] = {
  "Ready", "Starting", "Fill", "Drain", "Rinse", "Save"
};

bool systemBusy() { return activeState != stIdle || manualWashActive; }

void getSystemStatus(SystemStatus& s) {
  s.outputs       = outputsBits();
  s.state         = activeState;
  s.stateName     = (activeState >= 0 && activeState < STATE_COUNT) ? STATE_LABELS[activeState] : "Unknown";
  s.title         = displayGetTitle();
  s.message       = displayGetDetail();
  s.tankLevel     = tankLevel();
  s.tankFault     = tankFault();
  s.washValveOpen = outputsWashOpen();
  s.tapLpm        = tapFlow().rateLpm();
  s.drainLpm      = drainFlow().rateLpm();
  s.tapPulses     = tapPulseCount;
  s.drainPulses   = drainPulseCount;
  s.tapFlowing    = tapFlow().flowing();
  s.drainFlowing  = drainFlow().flowing();
  bool fillStep   = (activeState == stFill1 || activeState == stFill2);
  s.stepFillL     = fillStep ? gates.litresTaken(tapFlow()) : 0.0f;
  s.fillComplete  = fillStep && gates.filled;
  s.stepElapsedS  = stepStartTime ? (millis() - stepStartTime) / 1000UL : 0;
  s.drain2LeftS   = 0;
  if (activeState == stDrain2) {
    unsigned long elapsed = millis() - drain2StartTime;
    s.drain2LeftS = (elapsed < cfg.drain2DurationMs) ? (cfg.drain2DurationMs - elapsed) / 1000UL : 0;
  }
  s.stuck         = stepAlertSent && activeState >= stFill1 && activeState <= stFill2;
  s.tankAgeH      = tankAgeHours();
  s.lastAgoS      = -1;
  if (stats.lastValid) {
    if (stats.lastEndEpoch != 0 && timeIsValid())
      s.lastAgoS = (int32_t)(time(nullptr) - (time_t)stats.lastEndEpoch);
    else if (lastEndMillis != 0)
      s.lastAgoS = (int32_t)((millis() - lastEndMillis) / 1000UL);
  }
  s.cycleTapL     = cycleTapL;
  s.cycleReusedL  = cycleReusedL;
  s.cycleSavedL   = cycleSavedL;
  s.noSave        = cycleActive && cycleNoSave;
  s.manualWash    = manualWashActive;
  s.manualWashResult = manualWashResult;
  s.manualWashLeftS = 0;
  if (manualWashActive) {
    unsigned long elapsed = millis() - manualWashStart;
    s.manualWashLeftS = (elapsed < cfg.manualWashMs) ? (cfg.manualWashMs - elapsed) / 1000UL : 0;
  }
}

