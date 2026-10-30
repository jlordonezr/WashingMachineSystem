// Tests for TankLevel.h (float-switch debouncing and tank decisions).
// g++ -std=c++17 -O1 -Wall test_level.cpp -o test_level && ./test_level
#include "../TankLevel.h"
#include <cstdio>

static int passes = 0, failures = 0;
#define CHECK(c, ...) do { if (c) ++passes; else { ++failures; printf("  FAIL %s: ", __func__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Same timings as the firmware: safe reading after 200 ms, the other after 2 s, fault after 3 s.
static void setup(TankLevel& t) { t.configure(200, 2000, 3000); }

// Feeds the same raw values for ms milliseconds in 10 ms steps; returns the time reached.
static uint32_t feed(TankLevel& t, bool lo, bool hi, uint32_t from, uint32_t ms) {
  uint32_t x = from;
  for (uint32_t done = 0; done < ms; done += 10, x += 10) t.update(lo, hi, x);
  return x;
}

// L1: the first reading is taken at once (no wait at power-up).
static void L1() {
  TankLevel t; setup(t); t.update(true, false, 0);
  CHECK(t.hasWater() && !t.full() && t.level() == TankLevel::Medium, "first reading not used");
}
// L2: full is noticed fast (0.2 s) and cleared slowly (2 s).
static void L2() {
  TankLevel t; setup(t); uint32_t n = feed(t, true, false, 0, 100);
  n = feed(t, true, true, n, 150);   CHECK(!t.full(), "high float counted after 0.15 s");
  n = feed(t, true, true, n, 100);   CHECK(t.full(), "high float not counted after 0.25 s");
  n = feed(t, true, false, n, 1900); CHECK(t.full(), "full cleared after 1.9 s dry");
  n = feed(t, true, false, n, 200);  CHECK(!t.full(), "full not cleared after 2.1 s dry");
}
// L3: a high float bobbing every 300 ms reads as full and stays full (no chatter).
static void L3() {
  TankLevel t; setup(t); uint32_t n = feed(t, true, false, 0, 100);
  int changes = 0; bool was = t.full();
  for (int i = 0; i < 50; ++i) {
    n = feed(t, true, true, n, 300);  if (t.full() != was) { ++changes; was = t.full(); }
    n = feed(t, true, false, n, 300); if (t.full() != was) { ++changes; was = t.full(); }
  }
  CHECK(t.full() && changes == 1, "bobbing float: full=%d, %d changes", t.full(), changes);
}
// L4: high wet + low dry = full, no water for the pump, and a fault after 3 s (the 2.9 bug).
static void L4() {
  TankLevel t; setup(t); uint32_t n = feed(t, false, true, 0, 1000);
  CHECK(t.full(), "high wet with low dry not treated as full");
  CHECK(!t.hasWater(), "pump allowed with the low float dry");
  CHECK(!t.fault(), "fault reported after only 1 s");
  feed(t, false, true, n, 2500);
  CHECK(t.fault(), "disagreement not reported after 3.5 s");
  CHECK(t.level() == TankLevel::High, "level should read High");
}
// L5: healthy readings map exactly as in 2.9.
static void L5() {
  TankLevel t; setup(t);
  uint32_t n = feed(t, false, false, 0, 3000); CHECK(t.level() == TankLevel::Low && !t.fault(), "empty");
  n = feed(t, true, false, n, 3000);           CHECK(t.level() == TankLevel::Medium && !t.fault(), "half");
  n = feed(t, true, true, n, 3000);            CHECK(t.level() == TankLevel::High && !t.fault(), "full");
}
// L6: the pump loses permission fast (0.2 s) when the low float goes dry, and gets it back slowly.
static void L6() {
  TankLevel t; setup(t); uint32_t n = feed(t, true, false, 0, 100);
  n = feed(t, false, false, n, 250); CHECK(!t.hasWater(), "pump still allowed 0.25 s after the low float went dry");
  n = feed(t, true, false, n, 1500); CHECK(!t.hasWater(), "pump allowed again after only 1.5 s");
  n = feed(t, true, false, n, 600);  CHECK(t.hasWater(), "pump not allowed after 2.1 s wet");
}
// L7: millis() wrap-around does not disturb debouncing.
static void L7() {
  TankLevel t; setup(t); uint32_t n = feed(t, true, false, 0xFFFFFF00u, 100);
  feed(t, true, true, n, 400); CHECK(t.full(), "change across the millis() wrap not counted");
}
// L8: a short disagreement while the level passes the marks is not a fault.
static void L8() {
  TankLevel t; setup(t); uint32_t n = feed(t, true, true, 0, 3000);
  n = feed(t, false, true, n, 1000); n = feed(t, true, true, n, 3000);
  CHECK(!t.fault(), "1 s disagreement reported as a fault");
}

int main() {
  L1(); L2(); L3(); L4(); L5(); L6(); L7(); L8();
  printf("%d checks passed, %d failed\n", passes, failures);
  return failures ? 1 : 0;
}
