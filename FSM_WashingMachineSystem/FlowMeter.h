#ifndef FLOW_METER_H
#define FLOW_METER_H

// Flow measurement and flow-start detection for the grey water controller.
//
// This file has no Arduino dependencies, so the same code runs on the board and
// in the PC test suite (tests/test_flow.cpp).
//
// Why it replaces the old detectRisingEdge():
//  - The old detector latched "detected" and only re-armed when it was itself
//    called with an exact zero reading. It was not called in every state, so a
//    trickle or a few noise pulses kept it latched and Fill2 never advanced.
//    Unplugging the sensor forced a zero, which is why that "fixed" it.
//  - Any single pulse in a 250 ms window counted as flow, and one empty window
//    reset the 4 s timer, so noise looked like flow and slow flow never counted.
//
// FlowMeter instead:
//  - measures the rate over a 2 s sliding window (8 x 250 ms samples); 2 s,
//    not 1 s, so a slow trickle is resolved finely enough (about 0.09 L/min
//    per pulse on the YF-B10) to stay clearly below the stop threshold,
//  - says "flowing" only above onLpm for confirmMs, tolerating gaps up to gapMs,
//  - says "stopped" only below offLpm for stopMs. The firmware uses
//    offLpm = onLpm: the 2 s stop time prevents flicker, and a lower stop level
//    would let steady noise just under the threshold keep a drain "flowing",
//  - is updated on every loop() pass, so its state is always current,
//  - counts flow starts (edges); a state remembers the count when it begins
//    (CycleGates::enter) and looks for a newer one, so nothing ever stays latched.

#include <stdint.h>

class FlowMeter {
public:
  static const uint8_t  WINDOW   = 8;     // samples in the sliding window (2 s)
  static const uint32_t SAMPLE_MS = 250;  // sample period

  explicit FlowMeter(float pulsesPerLitre) : ppl_(pulsesPerLitre) {}

  void configure(float onLpm, float offLpm, uint32_t confirmMs, uint32_t gapMs, uint32_t stopMs) {
    onLpm_ = onLpm; offLpm_ = (offLpm <= onLpm) ? offLpm : onLpm;
    confirmMs_ = confirmMs; gapMs_ = gapMs; stopMs_ = stopMs;
  }

  // Call often (every loop). pulseCount = running total from the interrupt;
  // it may wrap around at 2^32. Returns the litres measured since the last
  // sample (0 between samples), for water accounting.
  float update(uint32_t pulseCount, uint32_t nowMs) {
    if (!started_) {
      started_ = true; lastCount_ = pulseCount; lastSampleMs_ = nowMs;
      for (uint8_t i = 0; i < WINDOW; ++i) { winPulses_[i] = 0; winMs_[i] = SAMPLE_MS; }
      return 0.0f;
    }
    uint32_t dt = nowMs - lastSampleMs_;
    if (dt < SAMPLE_MS) return 0.0f;

    uint32_t pulses = pulseCount - lastCount_;           // wrap-safe
    lastCount_ = pulseCount; lastSampleMs_ = nowMs;
    if (dt > 10 * SAMPLE_MS) dt = 10 * SAMPLE_MS;         // after a long stall, cap the window time

    winPulses_[idx_] = pulses; winMs_[idx_] = dt; idx_ = (idx_ + 1) % WINDOW;
    uint32_t p = 0, ms = 0;
    for (uint8_t i = 0; i < WINDOW; ++i) { p += winPulses_[i]; ms += winMs_[i]; }
    rateLpm_ = (ms > 0) ? (p / ppl_) * 60000.0f / ms : 0.0f;

    step(nowMs);
    float litres = pulses / ppl_;
    totalL_ += litres;
    return litres;
  }

  float    rateLpm() const { return rateLpm_; }
  bool     flowing() const { return flowing_; }
  uint32_t starts()  const { return starts_; }   // number of confirmed flow starts so far
  float    totalLitres() const { return totalL_; } // litres measured since boot

private:
  void step(uint32_t now) {
    if (!flowing_) {
      if (rateLpm_ >= onLpm_) {
        if (!candidate_) { candidate_ = true; aboveSince_ = now; }
        gapActive_ = false;
        if (now - aboveSince_ >= confirmMs_) { flowing_ = true; candidate_ = false; ++starts_; }
      } else if (candidate_) {
        if (!gapActive_) { gapActive_ = true; gapSince_ = now; }
        if (now - gapSince_ > gapMs_) { candidate_ = false; gapActive_ = false; }
      }
    } else {
      if (rateLpm_ < offLpm_) {
        if (!belowActive_) { belowActive_ = true; belowSince_ = now; }
        if (now - belowSince_ >= stopMs_) { flowing_ = false; belowActive_ = false; }
      } else {
        belowActive_ = false;
      }
    }
  }

  float    ppl_;
  float    onLpm_ = 1.0f, offLpm_ = 0.5f;
  uint32_t confirmMs_ = 4000, gapMs_ = 1000, stopMs_ = 2000;

  bool     started_ = false;
  uint32_t lastCount_ = 0, lastSampleMs_ = 0;
  uint32_t winPulses_[WINDOW] = {0};
  uint32_t winMs_[WINDOW]     = {SAMPLE_MS, SAMPLE_MS, SAMPLE_MS, SAMPLE_MS,
                                 SAMPLE_MS, SAMPLE_MS, SAMPLE_MS, SAMPLE_MS};
  uint8_t  idx_ = 0;
  float    rateLpm_ = 0.0f;
  float    totalL_  = 0.0f;

  bool     flowing_ = false;
  uint32_t starts_ = 0;
  bool     candidate_ = false;   uint32_t aboveSince_ = 0;
  bool     gapActive_ = false;   uint32_t gapSince_ = 0;
  bool     belowActive_ = false; uint32_t belowSince_ = 0;
};

// What each step of the cycle waits for. A step calls enter() when it begins;
// the "...Done" functions then answer from the meters, never from stale flags.
//
// Steps 1 and 3 ("fill, then drain") use a "machine is filled" guard: a drain
// only counts if it STARTS after the machine has
//   1. taken water in this step (flow already running when the step began counts),
//   2. taken at least minLitres (filters out splashes and detergent flushes), and
//   3. stopped taking water (its inlet closed: the fill is finished).
// Without this, a drain seen before or during the fill - many machines run the
// drain pump at program start, or interference on the drain wire - ended the
// step the moment the fill was detected (fixed in 2.6; tests S9-S12).
struct CycleGates {
  uint32_t tapArm = 0, drainArm = 0;
  float    litresArm = 0.0f;
  bool     tapSeen = false, filled = false, drainSeen = false;

  void enter(const FlowMeter& tap, const FlowMeter& drain) {
    tapArm = tap.starts(); drainArm = drain.starts(); litresArm = tap.totalLitres();
    tapSeen = false; filled = false; drainSeen = false;
  }
  // Water the machine has taken since this step began.
  float litresTaken(const FlowMeter& tap) const { return tap.totalLitres() - litresArm; }

  bool fillThenDrainDone(const FlowMeter& tap, const FlowMeter& drain, float minLitres) {
    tapSeen = tapSeen || tap.flowing() || tap.starts() > tapArm;
    if (!filled && tapSeen && !tap.flowing() && litresTaken(tap) >= minLitres) {
      filled = true;
      drainArm = drain.starts();          // only drains that start from now on count
    }
    drainSeen = drainSeen || (filled && drain.starts() > drainArm);
    return drainSeen;
  }
  // Step 1: the machine fills (wash water), then starts draining it.
  bool fill1Done(const FlowMeter& tap, const FlowMeter& drain, float minLitres) {
    return fillThenDrainDone(tap, drain, minLitres);
  }
  // Step 2: a NEW fill starts (the rinse fill).
  bool drain1Done(const FlowMeter& tap) {
    tapSeen = tapSeen || tap.starts() > tapArm;
    return tapSeen;
  }
  // Step 3: the rinse fill finishes, then a NEW drain starts (the rinse drain).
  // A drain still running from step 2 never counts, so soapy water is not saved.
  bool fill2Done(const FlowMeter& tap, const FlowMeter& drain, float minLitres) {
    return fillThenDrainDone(tap, drain, minLitres);
  }
};

// Protects the tank pump from running against a closed inlet (dead-heading).
//
// FlowMeter is deliberately slow (2 s average, 2 s stop delay) so that steps
// are judged reliably. That is far too slow to switch a pump off: when the
// washing machine closes its inlet valve, the pump would keep pushing against
// it for about 2 s. PumpGuard uses the time since the last sensor pulse
// instead, which reacts within noFlowMs (300 ms by default; at fill rates of
// 5-10 L/min pulses arrive every 10-20 ms).
//
//  - allow():    asked by the fill logic every 250 ms; starts the pump when
//                tank water is wanted, unless a cut-off is still blocking it.
//  - mustStop(): asked on every loop pass while the pump runs; true means
//                "no flow: switch the pump off now".
//  - After graceMs from start (pump spinning up, tap valve closing), no pulse
//    for noFlowMs stops the pump. It then stays off until the machine starts a
//    NEW draw, so it never restarts straight into the closed inlet while the
//    slow meter still reads "flowing".
//  - A stop with no pulse at all since the pump started is a dry start (air
//    lock, blocked intake): counted so the firmware can raise an alert.
class PumpGuard {
public:
  void configure(uint32_t graceMs, uint32_t noFlowMs) { graceMs_ = graceMs; noFlowMs_ = noFlowMs; }

  bool allow(bool wantTankWater, uint32_t flowStarts, uint32_t nowMs) {
    if (blocked_ && flowStarts > blockedAtStarts_) blocked_ = false;   // a new draw began
    if (!wantTankWater || blocked_) { running_ = false; return false; }
    if (!running_) { running_ = true; startMs_ = nowMs; pulseSinceStart_ = false; }
    return true;
  }

  // lastPulseMs = millis() of the most recent sensor pulse.
  bool mustStop(uint32_t lastPulseMs, uint32_t flowStarts, uint32_t nowMs) {
    if (!running_) return false;
    if ((int32_t)(lastPulseMs - startMs_) > 0) pulseSinceStart_ = true;
    if (nowMs - startMs_ < graceMs_) return false;
    uint32_t quiet = (int32_t)(lastPulseMs - startMs_) > 0 ? nowMs - lastPulseMs : nowMs - startMs_;
    if (quiet < noFlowMs_) return false;
    running_ = false; blocked_ = true; blockedAtStarts_ = flowStarts;
    lastStopDry_ = !pulseSinceStart_;
    if (lastStopDry_) ++dryStarts_;
    return true;
  }

  void reset()           { running_ = false; blocked_ = false; }   // new cycle
  void clearDryStarts()  { dryStarts_ = 0; }
  bool     running()     const { return running_; }
  bool     lastStopDry() const { return lastStopDry_; }
  uint32_t dryStarts()   const { return dryStarts_; }

private:
  uint32_t graceMs_ = 1000, noFlowMs_ = 300;
  bool     running_ = false, blocked_ = false, pulseSinceStart_ = false, lastStopDry_ = false;
  uint32_t startMs_ = 0, blockedAtStarts_ = 0, dryStarts_ = 0;
};

// Interrupt glitch filter: pulses closer than minUs apart are electrical noise
// (the fastest real pulse train, about 150 Hz, is 6.7 ms apart).
inline bool flowPulseAccept(uint32_t nowUs, uint32_t& lastUs, uint32_t minUs) {
  if (nowUs - lastUs < minUs) return false;
  lastUs = nowUs;
  return true;
}

#endif
