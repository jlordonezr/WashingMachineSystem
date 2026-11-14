#ifndef TANK_VOLUME_H
#define TANK_VOLUME_H

// Estimated litres in the storage tank (3.13).
//
// Like TankLevel.h and FlowMeter.h this file has no Arduino dependencies, so the
// same code runs on the board and in the PC tests (tests/test_tank_volume.cpp).
//
// The tank has no level gauge, only two float switches, so the volume is
// worked out from the water the controller moves:
//   - out: what the pump draws for the machine (tap flow sensor, YF-B1, while
//          the machine fills from the tank),
//   - in:  rinse water saved in step 4 (drain flow sensor, YF-B10, while the
//          fill valve is open), and tank-wash water while the wash valve is
//          open (not measured: an assumed rate, washLpm),
// and it is corrected by the floats whenever they say something definite:
//   - high float wet: at least highMarkL; the moment it gets wet, exactly highMarkL;
//   - high float dry: at most highMarkL;
//   - low float wet:  at least lowMarkL;
//   - low float dry:  at most lowMarkL; the moment it goes dry, exactly lowMarkL.
// It is always kept within 0 .. capacityL.

#include <math.h>
#include <stdint.h>

class TankVolume {
public:
  enum Trend : int8_t { Draining = -1, Steady = 0, Filling = 1 };

  void configure(float capacityL, float lowMarkL, float highMarkL, float washLpm) {
    cap_ = capacityL; low_ = lowMarkL; high_ = highMarkL; washLpm_ = washLpm;
  }

  // A starting value: the saved estimate, or a guess from the floats.
  void set(float litres) { v_ = clamp(litres); known_ = true; }
  void startFromFloats(bool lowWet, bool highWet) {
    set(highWet ? high_ : (lowWet ? (low_ + high_) / 2 : low_ / 2));
  }

  // Water moved since the last call (litres, >= 0) and how long the wash valve
  // was open (ms). dtMs is the time since the last call, for the trend.
  void flow(float inL, float outL, uint32_t washOpenMs, uint32_t dtMs) {
    float d = inL - outL + washLpm_ * washOpenMs / 60000.0f;
    v_ = clamp(v_ + d);
    // Trend: litres per second, smoothed over about 2 s.
    if (dtMs > 0) {
      float rate = d * 1000.0f / dtMs;
      float k = dtMs / (2000.0f + dtMs);
      rate_ += (rate - rate_) * k;
    }
  }

  // The debounced float switches (TankLevel): call every pass.
  void floats(bool lowWet, bool highWet) {
    if (!seen_) { seen_ = true; lastLow_ = lowWet; lastHigh_ = highWet; }
    if (highWet && !lastHigh_) v_ = high_;            // the level has just reached the high float
    if (!lowWet && lastLow_)   v_ = low_;             // ... has just dropped below the low float
    if (highWet && v_ < high_) v_ = high_;
    if (!highWet && v_ > high_) v_ = high_;
    if (lowWet && v_ < low_)   v_ = low_;
    if (!lowWet && v_ > low_)  v_ = low_;
    lastLow_ = lowWet; lastHigh_ = highWet;
    v_ = clamp(v_);
  }

  float litres() const { return v_; }
  bool  known()  const { return known_; }
  float rateLps() const { return rate_; }
  Trend trend() const { return rate_ > 0.02f ? Filling : (rate_ < -0.02f ? Draining : Steady); }

private:
  float clamp(float v) const { return v < 0 ? 0 : (v > cap_ ? cap_ : v); }
  float cap_ = 100, low_ = 15, high_ = 90, washLpm_ = 6;
  float v_ = 0, rate_ = 0;
  bool  known_ = false, seen_ = false, lastLow_ = false, lastHigh_ = false;
};

#endif
