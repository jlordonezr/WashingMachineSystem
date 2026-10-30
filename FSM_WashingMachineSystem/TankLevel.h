#ifndef TANK_LEVEL_H
#define TANK_LEVEL_H

// Storage-tank float switches: debouncing and the decisions that depend on them.
//
// Like FlowMeter.h, this file has no Arduino dependencies, so the same code runs
// on the board and in the PC tests (tests/test_level.cpp).
//
// Why (firmware 3.0, findings F2 and F3 of the 2.9 assessment):
//  - 2.9 read the switches raw on every 250 ms pass. A float bobbing at the high
//    mark switched the fill valve on and off every 250 ms during step 4.
//  - 2.9 mapped "high float wet, low float dry" to "low". During step 4 that kept
//    the fill valve open into a tank the high float reported as full.
//
// Rules now:
//  - Each switch moves quickly towards the SAFE reading and slowly away from it:
//      high float: wet after fastMs, dry only after slowMs  (full is noticed at once,
//                  and a bobbing float keeps the tank "full" instead of chattering)
//      low float:  dry after fastMs, wet only after slowMs  (the pump stops at once,
//                  and restarts only when the level is clearly back)
//  - The tank is FULL whenever the high float is wet, whatever the low float says.
//    Closing early is safe; overflowing is not.
//  - The tank HAS WATER for the pump only when the low float is wet. With a
//    disagreement (high wet, low dry) the pump is not used and tap water is.
//  - A disagreement that lasts faultMs is a sensor fault, reported once.

#include <stdint.h>

class LevelSwitch {
public:
  // toWetMs: how long the switch must read wet before it counts as wet; toDryMs likewise.
  void configure(uint32_t toWetMs, uint32_t toDryMs) { toWetMs_ = toWetMs; toDryMs_ = toDryMs; }

  // raw = true when the switch reports water. Call often (every pass).
  bool update(bool raw, uint32_t nowMs) {
    if (!init_) { init_ = true; state_ = cand_ = raw; since_ = nowMs; return state_; }
    if (raw != cand_) { cand_ = raw; since_ = nowMs; }
    else if (cand_ != state_ && nowMs - since_ >= (cand_ ? toWetMs_ : toDryMs_)) state_ = cand_;
    return state_;
  }
  bool wet() const { return state_; }

private:
  uint32_t toWetMs_ = 1000, toDryMs_ = 1000, since_ = 0;
  bool     init_ = false, state_ = false, cand_ = false;
};

class TankLevel {
public:
  enum Level : uint8_t { Low = 0, Medium = 1, High = 2 };

  void configure(uint32_t fastMs, uint32_t slowMs, uint32_t faultMs) {
    hi_.configure(fastMs, slowMs);   // full: noticed fast, cleared slowly
    lo_.configure(slowMs, fastMs);   // water for the pump: granted slowly, withdrawn fast
    faultMs_ = faultMs;
  }
  void update(bool lowRaw, bool highRaw, uint32_t nowMs) {
    lo_.update(lowRaw, nowMs); hi_.update(highRaw, nowMs);
    bool disagree = hi_.wet() && !lo_.wet();
    if (!disagree) { disagreeSince_ = 0; disagreeActive_ = false; fault_ = false; }
    else if (!disagreeActive_) { disagreeActive_ = true; disagreeSince_ = nowMs; }
    else if (nowMs - disagreeSince_ >= faultMs_) fault_ = true;
  }

  bool  hasWater() const { return lo_.wet(); }   // the pump may draw from the tank
  bool  full()     const { return hi_.wet(); }   // stop adding water to the tank
  bool  fault()    const { return fault_; }      // switches have disagreed for faultMs
  Level level()    const { return full() ? High : (hasWater() ? Medium : Low); }

private:
  LevelSwitch lo_, hi_;
  uint32_t    faultMs_ = 2000, disagreeSince_ = 0;
  bool        disagreeActive_ = false, fault_ = false;
};

#endif
