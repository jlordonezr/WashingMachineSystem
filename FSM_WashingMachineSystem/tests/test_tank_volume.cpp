// Estimated tank volume (TankVolume.h, 3.13): litres in and out, the wash
// valve's assumed rate, the 0-100 L limits, the float corrections, the trend.
#include "../TankVolume.h"
#include <stdio.h>

static int passed = 0, failed = 0;
#define CHECK(cond, ...) do { if (cond) passed++; else { failed++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static bool near(float a, float b, float tol = 0.01f) { return fabsf(a - b) <= tol; }

int main() {
  printf("Volume from measured litres\n");
  { TankVolume t; t.configure(100, 15, 90, 6); t.set(50);
    t.floats(true, false);
    t.flow(10, 0, 0, 1000);  CHECK(near(t.litres(), 60), "10 L saved: %.2f, expected 60", t.litres());
    t.flow(0, 25, 0, 1000);  CHECK(near(t.litres(), 35), "25 L pumped out: %.2f, expected 35", t.litres());
    t.flow(0, 0, 60000, 60000); CHECK(near(t.litres(), 41), "wash valve open 1 min at 6 L/min: %.2f, expected 41", t.litres()); }

  printf("Limits\n");
  { TankVolume t; t.configure(100, 15, 90, 6); t.set(5);
    t.flow(0, 20, 0, 1000); CHECK(near(t.litres(), 0), "below empty: %.2f, expected 0", t.litres());
    t.set(250); CHECK(near(t.litres(), 100), "set above capacity: %.2f, expected 100", t.litres());
    t.flow(30, 0, 0, 1000); CHECK(near(t.litres(), 100), "above capacity: %.2f, expected 100", t.litres()); }

  printf("Float corrections\n");
  { TankVolume t; t.configure(100, 15, 90, 6);
    t.set(70); t.floats(true, false);
    t.floats(true, true);  CHECK(near(t.litres(), 90), "high float gets wet at 70 L estimated: %.2f, expected 90", t.litres());
    t.flow(5, 0, 0, 1000); t.floats(true, true); CHECK(near(t.litres(), 95), "above the high float, still counted: %.2f", t.litres());
    t.floats(true, false); CHECK(near(t.litres(), 90), "high float dry: at most 90: %.2f", t.litres());
    t.set(8); t.floats(true, false); CHECK(near(t.litres(), 15), "low float wet: at least 15: %.2f", t.litres());
    t.set(40); t.floats(true, false); t.floats(false, false); CHECK(near(t.litres(), 15), "low float goes dry at 40 L estimated: %.2f, expected 15", t.litres());
    t.flow(0, 10, 0, 1000); t.floats(false, false); CHECK(near(t.litres(), 5), "below the low float, still counted: %.2f", t.litres());
    t.set(60); t.floats(false, false); CHECK(near(t.litres(), 15), "low float dry: at most 15: %.2f", t.litres()); }

  printf("Start-up guess from the floats\n");
  { TankVolume t; t.configure(100, 15, 90, 6);
    t.startFromFloats(true, true);   CHECK(near(t.litres(), 90), "both wet: %.1f, expected 90", t.litres());
    t.startFromFloats(true, false);  CHECK(near(t.litres(), 52.5f), "low wet only: %.1f, expected 52.5", t.litres());
    t.startFromFloats(false, false); CHECK(near(t.litres(), 7.5f), "both dry: %.1f, expected 7.5", t.litres());
    CHECK(t.known(), "not marked known after a start value"); }

  printf("Trend (fill / drain arrow)\n");
  { TankVolume t; t.configure(100, 15, 90, 6); t.set(50);
    CHECK(t.trend() == TankVolume::Steady, "steady at start");
    for (int i = 0; i < 40; i++) t.flow(0.15f / 4, 0, 0, 250);     // 9 L/min in
    CHECK(t.trend() == TankVolume::Filling, "saving 9 L/min: trend %d, rate %.3f L/s", t.trend(), t.rateLps());
    for (int i = 0; i < 40; i++) t.flow(0, 0.13f / 4, 0, 250);     // 8 L/min out
    CHECK(t.trend() == TankVolume::Draining, "pumping 8 L/min: trend %d", t.trend());
    for (int i = 0; i < 40; i++) t.flow(0, 0, 0, 250);
    CHECK(t.trend() == TankVolume::Steady, "10 s with no flow: trend %d", t.trend()); }

  printf("%d checks passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
