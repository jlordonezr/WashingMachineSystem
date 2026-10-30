#include "GreyWaterOutputs.h"
#include "GreyWaterSystem.h"
#include "GreyWaterSettings.h"

// See GreyWaterOutputs.h. Everything below that touches shared state runs
// with gwLock held: either inside safetyTick() or inside a public function
// that takes the lock itself.

// ── State (guarded by gwLock) ──────────────────────────────────────────────
static RelayMode requested = modeOff;     // what the cycle asked for
static RelayMode effective = modeOff;     // what the pins are set to
static uint8_t   bits = 0;                // pin states, see outputsBits()
static bool      pinsWritten = false;     // the first apply always writes

static TankLevel tank;
static bool      highRaw = false;

static PumpGuard pump;
static uint32_t  tapStartsSeen = 0;       // tap meter flow starts, passed in by loop()
static uint32_t  tapCountSeen = 0;        // pulse count at the last tick
static uint32_t  tapLastPulseMs = 0;      // millis() when the count last changed

static bool          tankWashStarted = false;
static unsigned long tankWashStartTime = 0;

static SafetyEvents events = {};
static bool         eventsPending = false;

// ── Pin writing ────────────────────────────────────────────────────────────
void controlSSR(int pin, int control, int wait) {
  bool turnOn = (control == HIGH);
  DEBUG_PRINT(turnOn ? "SSR ON" : "SSR OFF");
  digitalWrite(pin, turnOn ? ssrON : ssrOFF);
  if (wait > 0) delay(wait);
}

static void writePins(uint8_t b) {
  if (pinsWritten && b == bits) return;     // nothing changed: no pin writes
  pinsWritten = true;
  bits = b;
  controlSSR(OUT_TAP_EV,   (b & 1)  ? HIGH : LOW, CONTRL_SSR_DELAY);
  controlSSR(OUT_DRAIN_EV, (b & 2)  ? HIGH : LOW, CONTRL_SSR_DELAY);
  controlSSR(OUT_FILL_EV,  (b & 4)  ? HIGH : LOW, CONTRL_SSR_DELAY);
  controlSSR(OUT_MOTOR,    (b & 8)  ? HIGH : LOW, CONTRL_SSR_DELAY);
  controlSSR(OUT_WASH_EV,  (b & 16) ? HIGH : LOW, CONTRL_SSR_DELAY);
}

// Tank-wash valve during a tank-water fill: opens every washPeriodMs for washOpenMs.
static bool tankWashPulse() {
  if (!tankWashStarted) { tankWashStarted = true; tankWashStartTime = millis(); }
  unsigned long phase = (millis() - tankWashStartTime) % cfg.washPeriodMs;
  return phase < cfg.washOpenMs;
}

// Interlocks: the requested mode, made safe for the current tank state.
static RelayMode safeMode(RelayMode m) {
  if (m == modeTankFill && !tank.hasWater()) return modeTapFill;   // no pump with the low float dry
  if (m == modeSaveTank && tank.full())      return modeStandby;   // no saving into a full tank
  if (m == modeManualWash && (tank.full() || highRaw)) return modeStandby;
  return m;
}

// Works out the pins for the effective mode and writes them. Lock held.
static void applyLocked() {
  effective = safeMode(requested);
  //                      tap   drain  fill  pump  wash
  uint8_t b = 0;
  switch (effective) {
    case modeOff:        b = 0;                    break;
    case modeStandby:
    case modeTapFill:
    case modeDrainSoapy: b = 1 | 2;                break;
    case modeTankFill:   b = 2 | 8;                break;
    case modeSaveTank:   b = 1 | 4;                break;
    case modeManualWash: b = 1 | 2 | 16;           break;
  }
  if (effective == modeTankFill && !(tank.full() || highRaw) && tankWashPulse()) b |= 16;
  writePins(b);
}

// ── Safety task body ───────────────────────────────────────────────────────
void safetyTick() {
  GwLockGuard g;
  uint32_t now = millis();

  highRaw = digitalRead(IN_HIGHLEVEL) == HIGH;
  tank.configure(LEVEL_FAST_MS, LEVEL_SLOW_MS, LEVEL_FAULT_MS);
  tank.update(digitalRead(IN_LOWLEVEL) == HIGH, highRaw, now);

  // Pump cut-off: no tap-sensor pulse for PUMP_NO_FLOW_MS while the pump runs.
  uint32_t count = tapPulseCount;                       // 32-bit reads are atomic on the ESP32
  if (count != tapCountSeen) { tapCountSeen = count; tapLastPulseMs = now; }
  pump.configure(PUMP_START_GRACE_MS, PUMP_NO_FLOW_MS);
  if (effective == modeTankFill && pump.mustStop(tapLastPulseMs, tapStartsSeen, now)) {
    requested = modeTapFill;                            // the tap covers any remaining draw
    events.pumpStopped = true;
    events.pumpStoppedDry = pump.lastStopDry();
    events.dryStarts = pump.dryStarts();
    eventsPending = true;
  }

  // Rewrite every pin about once a second even if nothing changed, as 2.9 did
  // on every 250 ms pass: a pin disturbed by anything else is corrected.
  static uint8_t ticks = 0;
  if (++ticks >= 1000 / SAFETY_TICK_MS) { ticks = 0; pinsWritten = false; }

  RelayMode before = effective;
  applyLocked();                                        // interlocks, wash pulses
  if (requested == modeManualWash && before == modeManualWash && effective != modeManualWash) {
    events.washClosedFull = true;                       // high float closed the manual wash
    eventsPending = true;
  }
}

// ── Public API ─────────────────────────────────────────────────────────────
void outputsBegin() {
  const int pins[] = { OUT_TAP_EV, OUT_DRAIN_EV, OUT_FILL_EV, OUT_MOTOR, OUT_WASH_EV };
  for (int p : pins) pinMode(p, OUTPUT);
  GwLockGuard g;
  // First reading of the floats, so the interlocks are right from the start.
  highRaw = digitalRead(IN_HIGHLEVEL) == HIGH;
  tank.configure(LEVEL_FAST_MS, LEVEL_SLOW_MS, LEVEL_FAULT_MS);
  tank.update(digitalRead(IN_LOWLEVEL) == HIGH, highRaw, millis());
  requested = modeOff;
  pinsWritten = false;
  applyLocked();
}

void outputsRequest(RelayMode mode) {
  GwLockGuard g;
  requested = mode;
  applyLocked();
}

void outputsAllOff()          { outputsRequest(modeOff); }
RelayMode outputsMode()       { GwLockGuard g; return effective; }
uint8_t outputsBits()         { GwLockGuard g; return bits; }
bool outputsWashOpen()        { GwLockGuard g; return (bits & 16) != 0; }
void resetTankWash()          { GwLockGuard g; tankWashStarted = false; }

bool pumpAllow(bool wantTankWater) {
  GwLockGuard g;
  return pump.allow(wantTankWater && tank.hasWater(), tapStartsSeen, millis());
}
void pumpResetForStep()  { GwLockGuard g; pump.reset(); }
void pumpResetForCycle() { GwLockGuard g; pump.clearDryStarts(); }
void pumpNoteFlowStarts(uint32_t tapStarts) { GwLockGuard g; tapStartsSeen = tapStarts; }

bool safetyTakeEvents(SafetyEvents& e) {
  GwLockGuard g;
  if (!eventsPending) return false;
  e = events;
  events = SafetyEvents{};
  eventsPending = false;
  return true;
}

bool    tankHasWater()   { GwLockGuard g; return tank.hasWater(); }
bool    tankFull()       { GwLockGuard g; return tank.full(); }
bool    tankFault()      { GwLockGuard g; return tank.fault(); }
bool    tankHighRawWet() { GwLockGuard g; return highRaw; }
uint8_t tankLevelRaw()   { GwLockGuard g; return (uint8_t)tank.level(); }
