#include "AgileStateMachine.h"
#include <Arduino.h>
#include <Wire.h>
#include "GreyWaterSystem.h"
#include "GreyWaterSettings.h"
#include "GreyWaterNet.h"

// ── SSR control levels ─────────────────────────────────────────────────────
int ssrON  = HIGH;
int ssrOFF = LOW;

// ── Button debounce state ──────────────────────────────────────────────────
unsigned long lastStartButtonPressTime = 0;
unsigned long lastStopButtonPressTime  = 0;
bool lastStartButtonState = HIGH;
bool lastStopButtonState  = HIGH;

// ── Timing variables ───────────────────────────────────────────────────────
unsigned long lastCheckTime   = 0;

// ── FSM globals ────────────────────────────────────────────────────────────
StateMachine fsm;

bool inStart  = false;
bool inStop   = false;
bool filled1  = false;
bool drained1 = false;
bool filled2  = false;

State* states[STATE_COUNT];

constexpr const char* stateNames[STATE_COUNT] = {
  "Idle",
  "Run",
  "1st Fill",
  "1st Drain",
  "2nd Fill",
  "2nd Drain"
};

// ── Helpers ────────────────────────────────────────────────────────────────
static void allSSROff() {
  const int outputs[] = { OUT_TAP_EV, OUT_DRAIN_EV, OUT_FILL_EV, OUT_MOTOR, OUT_WASH_EV };
  for (int pin : outputs) {
    controlSSR(pin, ssrOFF, CONTRL_SSR_DELAY);
  }
}

static bool isResettableState(int idx) {
  return idx == stDrain1 || idx == stFill2 || idx == stDrain2;
}

// Holding STOP for WIFI_SETUP_HOLD_MS while the system is READY opens the
// Wi-Fi setup page, so the home network can be changed without a computer.
static void checkWifiSetupHold() {
  static unsigned long heldSince = 0;
  if (activeState == stIdle && inStop && !netPortalActive()) {
    if (heldSince == 0) heldSince = millis();
    else if (millis() - heldSince >= WIFI_SETUP_HOLD_MS) {
      heldSince = 0;
      netStartConfigPortal();
    }
  } else {
    heldSince = 0;
  }
}

static void resetFSMToIdle() {
  fsm.stop();
  fsm.setInitialState(states[stIdle]);
  fsm.start();
  onEnter();
  DEBUG_PRINT("FSM reset to Idle state.");
}

// ── setup() ───────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(9600);
  settingsBegin();   // load settings and water statistics from flash first

  flowSensorsBegin();   // flow sensor pins and interrupts (see FlowMeter.h)

  pinMode(START_BUTTON, INPUT_PULLUP);
  pinMode(STOP_BUTTON,  INPUT_PULLUP);
  pinMode(IN_LOWLEVEL,  INPUT);
  pinMode(IN_HIGHLEVEL, INPUT);
  pinMode(OUT_TAP_EV,   OUTPUT);
  pinMode(OUT_DRAIN_EV, OUTPUT);
  pinMode(OUT_FILL_EV,  OUTPUT);
  pinMode(OUT_MOTOR,    OUTPUT);
  pinMode(OUT_WASH_EV,  OUTPUT);

  allSSROff();
  outputsBegin();    // the outputs module owns the SSRs from here on (interlocks, see GreyWaterOutputs.h)

  Wire.setClock(400000);  // I2C Fast Mode: a full frame takes ~13 ms instead of ~50 ms
                           // at the default 100 kHz, so scrolling stays smooth and the
                           // loop() is not held up while the OLED is being refreshed.

  // No screen is not a reason to stop: the cycle, buttons and app still work.
  // bootChecks() logs it and sends an alert once Wi-Fi is up.
  if (!displayBegin()) {
    DEBUG_PRINT("SSD1306 not found: running without a screen");
  }

  // Border Collie chasing a butterfly (about 4 s), then "System ready" for 2 s.
  displayStartIntro(TRS("REUSE WATER", "REUSO DE AGUA"), TRS("System ready", "Sistema listo"), 2000);
  displayUpdate();
  setupStateMachine();
  netBegin();        // Wi-Fi connects in the background; the cycle never waits for it
  buzzerBegin();
  bootChecks();      // log the restart reason; alert if a cycle was cut short
  safetyBegin();     // safety task: float switches, pump cut-off and interlocks every 20 ms, whatever loop() is doing
  enableLoopWDT();   // restart automatically (all outputs off) if loop() ever freezes for 5 s
}

// ── loop() ────────────────────────────────────────────────────────────────
void loop() {
  loopTimingMark();        // Service tab: time between passes
  updateInputs();          // buttons; reports a float-switch fault (the safety task reads the floats)
  applyRemoteRequests();   // dashboard Start/Stop act like a button press
  checkWifiSetupHold();

  if (isResettableState(fsm.getCurrentState()->getIndex()) && inStop) {
    resetFSMToIdle();
    return;
  }

  fsm.execute();

  switch (fsm.getCurrentState()->getIndex()) {
    case stIdle:   idleStateLogic();   break;
    case stRun:    runStateLogic();    break;
    case stFill1:  fill1StateLogic();  break;
    case stDrain1: drain1StateLogic(); break;
    case stFill2:  fill2StateLogic();  break;
    case stDrain2: drain2StateLogic(); break;
    default: break;
  }

  sampleFlowSensors();      // flow rates and water volumes, every pass
  systemPeriodic();         // tank level, water age alert
  displayUpdate();          // redraws the OLED only when needed (frame-timed, non-blocking)
  netLoop();                // Wi-Fi, dashboard, firmware updates (non-blocking)
}

// ── onEnter() ─────────────────────────────────────────────────────────────
void onEnter() {
  const int idx = fsm.getCurrentState()->getIndex();
  activeState = idx;
  DEBUG_PRINT("Entering state: ");
  DEBUG_PRINT(stateNames[idx]);

  switch (idx) {
    case stIdle:   handleIdleStateEntry();   break;
    case stRun:    handleRunStateEntry();    break;
    case stFill1:  handleFill1StateEntry();  break;
    case stDrain1: handleDrain1StateEntry(); break;
    case stFill2:  handleFill2StateEntry();  break;
    case stDrain2: handleDrain2StateEntry(); break;
    default:
      DEBUG_PRINT("Default state entry logic.");
      break;
  }
}

// ── setupStateMachine() ───────────────────────────────────────────────────
void setupStateMachine() {
  for (int i = 0; i < STATE_COUNT; ++i) {
    states[i] = fsm.addState(stateNames[i], onEnter);
  }

  states[stIdle]->addTransition(states[stRun],     []() { return inStart;  });
  states[stRun]->addTransition( states[stFill1],   OPERATION_DELAY);
  states[stFill1]->addTransition(states[stDrain1], []() { return filled1;  });
  states[stDrain1]->addTransition(states[stFill2], []() { return drained1; });
  states[stFill2]->addTransition(states[stDrain2], []() { return filled2;  });
  states[stDrain2]->addTransition(states[stIdle],  []() { return drain2Done; });  // length set in Settings

  for (int i = stRun; i <= stDrain2; ++i) {
    states[i]->addTransition(states[stIdle], []() { return inStop; });
  }

  fsm.setInitialState(states[stIdle]);
  fsm.start();
  onEnter();

  DEBUG_PRINT("Active state: ");
  DEBUG_PRINT(stateNames[fsm.getCurrentState()->getIndex()]);
}