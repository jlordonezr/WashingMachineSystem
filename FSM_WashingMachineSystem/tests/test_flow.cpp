// PC test suite for FlowMeter.h: flow measurement, cycle step detection and pump protection.
// Build and run:  g++ -std=c++17 -O1 -I.. test_flow.cpp -o test_flow && ./test_flow
// Exit code 0 = all tests passed.
#include "../FlowMeter.h"
#include <cstdio>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

static int failures = 0, passes = 0;
#define CHECK(cond, msg) do { if (cond) { ++passes; } else { ++failures; std::printf("    FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

// Firmware settings (defaults)
static const float    DRAIN_PPL = 352.0f;   // YF-B10
static const float    TAP_PPL   = 660.0f;   // YF-B1
static const float    ON_LPM = 1.0f, OFF_LPM = 1.0f;   // as configured by the firmware
static const uint32_t CONFIRM = 4000, GAP = 1000, STOP = 2000;
static const float    MIN_FILL = 3.0f;   // litres (firmware default)

// Firmware 2.5 step logic, kept to show the early Fill1 -> Drain1 fault (S9, S10).
struct OldGates {
  uint32_t tapArm = 0, drainArm = 0; bool tapSeen = false, drainSeen = false;
  void enter(const FlowMeter& tap, const FlowMeter& drain) { tapArm = tap.starts(); drainArm = drain.starts(); tapSeen = drainSeen = false; }
  bool fill1Done(const FlowMeter& tap, const FlowMeter& drain) {
    tapSeen = tapSeen || tap.flowing() || tap.starts() > tapArm;
    drainSeen = drainSeen || drain.starts() > drainArm;
    return tapSeen && drainSeen;
  }
};

// Simulated sensor: produces pulses for a flow profile (L/min over time),
// optionally with extra noise pulses. Time advances in 10 ms loop() passes.
struct Sim {
  uint32_t nowMs = 0, pulses = 0;
  double   acc = 0;
  float    ppl;
  explicit Sim(float p, uint32_t start = 0) : nowMs(start), pulses(0), ppl(p) {}
  void tick(float lpm, FlowMeter& m, float noiseHz = 0, uint32_t* noiseState = nullptr) {
    acc += lpm / 60.0 * ppl * 0.010;             // pulses in 10 ms
    while (acc >= 1.0) { acc -= 1.0; ++pulses; }
    if (noiseHz > 0 && noiseState) {             // deterministic noise pulses
      *noiseState += (uint32_t)(noiseHz * 10);   // per mille per 10 ms
      while (*noiseState >= 1000) { *noiseState -= 1000; ++pulses; }
    }
    m.update(pulses, nowMs);
    nowMs += 10;
  }
};
static FlowMeter makeMeter(float ppl) { FlowMeter m(ppl); m.configure(ON_LPM, OFF_LPM, CONFIRM, GAP, STOP); return m; }
static void run(Sim& s, FlowMeter& m, uint32_t ms, std::function<float(uint32_t)> lpm, float noiseHz = 0) {
  static uint32_t ns = 0;
  uint32_t end = s.nowMs + ms; uint32_t t0 = s.nowMs;
  while (s.nowMs < end) s.tick(lpm(s.nowMs - t0), m, noiseHz, &ns);
}
static void test(const char* name, std::function<void()> fn) {
  int before = failures; fn();
  std::printf("%s %s\n", failures == before ? "PASS" : "FAIL", name);
}

int main() {
  std::printf("FlowMeter tests (on %.1f L/min, off %.1f L/min, confirm %u ms, gap %u ms, stop %u ms)\n\n",
              ON_LPM, OFF_LPM, CONFIRM, GAP, STOP);

  test("T01 no pulses: never flowing, rate 0", [] {
    FlowMeter m = makeMeter(DRAIN_PPL); Sim s(DRAIN_PPL);
    run(s, m, 60000, [](uint32_t) { return 0.0f; });
    CHECK(!m.flowing(), "flowing with no pulses");
    CHECK(m.starts() == 0, "starts counted with no pulses");
    CHECK(m.rateLpm() == 0.0f, "rate not zero");
  });

  test("T02 steady 18 L/min drain: confirmed after 4 s, one start, rate within 5%", [] {
    FlowMeter m = makeMeter(DRAIN_PPL); Sim s(DRAIN_PPL);
    run(s, m, 3500, [](uint32_t) { return 18.0f; });
    CHECK(!m.flowing(), "confirmed before the 4 s confirm time");
    run(s, m, 1500, [](uint32_t) { return 18.0f; });
    CHECK(m.flowing(), "not confirmed after 5 s of steady flow");
    run(s, m, 20000, [](uint32_t) { return 18.0f; });
    CHECK(m.starts() == 1, "more than one start for one continuous flow");
    CHECK(std::fabs(m.rateLpm() - 18.0f) < 0.9f, "rate more than 5% off");
  });

  test("T03 noise at 1 pulse/s (0.17 L/min): never counts as flow", [] {
    FlowMeter m = makeMeter(DRAIN_PPL); Sim s(DRAIN_PPL);
    run(s, m, 120000, [](uint32_t) { return 0.0f; }, 1.0f);
    CHECK(!m.flowing() && m.starts() == 0, "noise counted as flow");
  });

  test("T04 trickle 0.4 L/min after a drain: meter returns to stopped", [] {
    FlowMeter m = makeMeter(DRAIN_PPL); Sim s(DRAIN_PPL);
    run(s, m, 10000, [](uint32_t) { return 18.0f; });
    run(s, m, 10000, [](uint32_t) { return 0.4f; });
    CHECK(!m.flowing(), "trickle below the off threshold kept it flowing");
  });

  test("T05 slow pulsed flow (2 L/min, 300 ms gaps every second): confirmed", [] {
    FlowMeter m = makeMeter(DRAIN_PPL); Sim s(DRAIN_PPL);
    run(s, m, 15000, [](uint32_t t) { return (t % 1000) < 700 ? 2.9f : 0.0f; });
    CHECK(m.flowing(), "pulsed flow above threshold not confirmed");
  });

  test("T06 flow, stop, flow again: two starts", [] {
    FlowMeter m = makeMeter(DRAIN_PPL); Sim s(DRAIN_PPL);
    run(s, m, 8000, [](uint32_t) { return 18.0f; });
    run(s, m, 5000, [](uint32_t) { return 0.0f; });
    CHECK(!m.flowing(), "did not stop after 5 s without flow");
    run(s, m, 8000, [](uint32_t) { return 18.0f; });
    CHECK(m.starts() == 2, "second flow not counted as a new start");
  });

  test("T07 short stop (1.5 s) during a drain is not a new start", [] {
    FlowMeter m = makeMeter(DRAIN_PPL); Sim s(DRAIN_PPL);
    run(s, m, 8000, [](uint32_t) { return 18.0f; });
    run(s, m, 1500, [](uint32_t) { return 0.0f; });
    run(s, m, 8000, [](uint32_t) { return 18.0f; });
    CHECK(m.starts() == 1, "a brief interruption counted as a new drain");
  });

  test("T08 burst shorter than the confirm time (2 s) is ignored", [] {
    FlowMeter m = makeMeter(DRAIN_PPL); Sim s(DRAIN_PPL);
    run(s, m, 2000, [](uint32_t) { return 18.0f; });
    run(s, m, 6000, [](uint32_t) { return 0.0f; });
    CHECK(m.starts() == 0, "2 s splash counted as flow");
  });

  test("T09 volume: 10 L/min for 60 s measures 10 L within 1%", [] {
    FlowMeter m(TAP_PPL); m.configure(ON_LPM, OFF_LPM, CONFIRM, GAP, STOP);
    uint32_t pulses = 0; double litres = 0; double acc = 0;
    for (uint32_t t = 0; t <= 60000; t += 10) {
      acc += 10.0 / 60.0 * TAP_PPL * 0.010; while (acc >= 1) { acc -= 1; ++pulses; }
      litres += m.update(pulses, t);
    }
    CHECK(std::fabs(litres - 10.0) < 0.1, "volume more than 1% off");
  });

  test("T10 pulse counter and millis() wrap-around", [] {
    FlowMeter m = makeMeter(DRAIN_PPL);
    uint32_t pulses = 0xFFFFFF00u, now = 0xFFFFF000u; double acc = 0;
    for (int i = 0; i < 1500; ++i) {               // 15 s across both wraps
      acc += 18.0 / 60.0 * DRAIN_PPL * 0.010; while (acc >= 1) { acc -= 1; ++pulses; }
      m.update(pulses, now); now += 10;
    }
    CHECK(m.flowing() && m.starts() == 1, "wrap-around broke detection");
    CHECK(std::fabs(m.rateLpm() - 18.0f) < 0.9f, "wrap-around broke the rate");
  });

  test("T12 steady noise 5 pulses/s (0.85 L/min) after a drain: returns to stopped (old code stayed latched)", [] {
    FlowMeter m = makeMeter(DRAIN_PPL); Sim s(DRAIN_PPL);
    run(s, m, 10000, [](uint32_t) { return 18.0f; });
    run(s, m, 10000, [](uint32_t) { return 0.0f; }, 5.0f);
    CHECK(!m.flowing(), "steady sub-threshold noise kept the drain flowing");
  });

  test("T11 glitch filter: spikes closer than 1 ms rejected, real pulses kept", [] {
    uint32_t last = 0, kept = 0;
    // 20 spikes 50 us apart (SSR switching transient), then 10 real pulses 7 ms apart
    uint32_t t = 100000;
    for (int i = 0; i < 20; ++i, t += 50) kept += flowPulseAccept(t, last, 1000);
    CHECK(kept == 1, "spike burst counted as more than one pulse");
    kept = 0; t += 7000;
    for (int i = 0; i < 10; ++i, t += 7000) kept += flowPulseAccept(t, last, 1000);
    CHECK(kept == 10, "real pulses rejected");
  });

  std::printf("\nCycle scenarios (CycleGates with both meters, as used by the state machine)\n\n");

  // Runs Fill1 -> Drain1 -> Fill2 with profiles for tap and drain. Returns the
  // second at which each step finished, or -1 if it did not within the limit.
  struct Result { int fill1 = -1, drain1 = -1, fill2 = -1; };
  auto cycle = [](std::function<float(uint32_t)> tapLpm, std::function<float(uint32_t)> drainLpm,
                  uint32_t limitMs, float drainNoiseHz, float onLpm = ON_LPM) {
    FlowMeter tap(TAP_PPL), drain(DRAIN_PPL);
    tap.configure(onLpm, onLpm, CONFIRM, GAP, STOP); drain.configure(onLpm, onLpm, CONFIRM, GAP, STOP);
    CycleGates g; Result r; int step = 1;
    uint32_t tp = 0, dp = 0; double ta = 0, da = 0; uint32_t noise = 0;
    tap.update(0, 0); drain.update(0, 0);
    g.enter(tap, drain);
    for (uint32_t t = 10; t < limitMs && step <= 3; t += 10) {
      ta += tapLpm(t) / 60.0 * TAP_PPL * 0.010;     while (ta >= 1) { ta -= 1; ++tp; }
      da += drainLpm(t) / 60.0 * DRAIN_PPL * 0.010; while (da >= 1) { da -= 1; ++dp; }
      noise += (uint32_t)(drainNoiseHz * 10);        while (noise >= 1000) { noise -= 1000; ++dp; }
      tap.update(tp, t); drain.update(dp, t);
      if (t % 250 != 0) continue;                    // state logic runs every 250 ms
      if (step == 1 && g.fill1Done(tap, drain, MIN_FILL))  { r.fill1 = t / 1000;  g.enter(tap, drain); step = 2; }
      else if (step == 2 && g.drain1Done(tap))   { r.drain1 = t / 1000; g.enter(tap, drain); step = 3; }
      else if (step == 3 && g.fill2Done(tap, drain, MIN_FILL))  { r.fill2 = t / 1000;  step = 4; }
    }
    return r;
  };
  // A typical machine: fill 0-120 s, wash, soapy drain 600-720 s, rinse fill 780-880 s, rinse drain 1500-1620 s.
  auto tapTypical   = [](uint32_t t) { return (t < 120000 || (t >= 780000 && t < 880000)) ? 8.0f : 0.0f; };
  auto drainTypical = [](uint32_t t) { return ((t >= 600000 && t < 720000) || (t >= 1500000 && t < 1620000)) ? 18.0f : 0.0f; };

  test("S1 typical cycle: every step advances at the right moment", [&] {
    Result r = cycle(tapTypical, drainTypical, 2000000, 0);
    std::printf("    fill1 done at %d s, drain1 at %d s, fill2 at %d s\n", r.fill1, r.drain1, r.fill2);
    CHECK(r.fill1 >= 600 && r.fill1 <= 606, "Fill1 did not end when the soapy drain started");
    CHECK(r.drain1 >= 780 && r.drain1 <= 786, "Drain1 did not end when the rinse fill started");
    CHECK(r.fill2 >= 1500 && r.fill2 <= 1506, "Fill2 did not end when the rinse drain started");
  });

  test("S2 reported bug: trickle between drains (old code stuck in Fill2)", [&] {
    auto drainTrickle = [&](uint32_t t) { float d = drainTypical(t); return d > 0 ? d : (t > 720000 ? 0.3f : 0.0f); };
    Result r = cycle(tapTypical, drainTrickle, 2000000, 0);
    CHECK(r.fill2 >= 1500 && r.fill2 <= 1506, "Fill2 stuck or early with a trickle between drains");
  });

  test("S2b steady pulse train 5/s between drains: the condition that stuck the old code", [&] {
    auto drainQuiet = [&](uint32_t t) { return drainTypical(t); };
    // noise only between the drains: emulate with a profile of 0.85 L/min outside the drains after 720 s
    auto drainNoisy = [&](uint32_t t) { float d = drainQuiet(t); return d > 0 ? d : (t > 720000 ? 0.85f : 0.0f); };
    Result r = cycle(tapTypical, drainNoisy, 2000000, 0);
    std::printf("    fill2 done at %d s\n", r.fill2);
    CHECK(r.fill2 >= 1500 && r.fill2 <= 1506, "Fill2 stuck with a steady sub-threshold pulse train");
  });

  test("S3 electrical noise on the drain line (2 pulses/s all cycle)", [&] {
    Result r = cycle(tapTypical, drainTypical, 2000000, 2.0f);
    CHECK(r.fill1 >= 600 && r.fill1 <= 606, "noise ended Fill1 early or blocked it");
    CHECK(r.fill2 >= 1500 && r.fill2 <= 1506, "noise blocked Fill2");
  });

  test("S4 slow gravity drain with gaps (old code never detected it)", [&] {
    auto drainSlow = [](uint32_t t) {
      bool on = (t >= 600000 && t < 720000) || (t >= 1500000 && t < 1620000);
      return (on && (t % 1000) < 700) ? 3.0f : 0.0f;
    };
    Result r = cycle(tapTypical, drainSlow, 2000000, 0);
    CHECK(r.fill1 > 0 && r.fill2 > 0, "slow pulsed drain not detected");
  });

  test("S5 machine already filling when START is pressed", [&] {
    auto tapEarly = [&](uint32_t t) { return (t < 60000 || (t >= 780000 && t < 880000)) ? 8.0f : 0.0f; };
    // tap meter already confirmed before the cycle: emulate by pre-flowing 10 s
    FlowMeter tap = makeMeter(TAP_PPL), drain = makeMeter(DRAIN_PPL);
    uint32_t tp = 0; double ta = 0;
    for (uint32_t t = 0; t < 10000; t += 10) { ta += 8.0 / 60 * TAP_PPL * 0.01; while (ta >= 1) { ta -= 1; ++tp; } tap.update(tp, t); drain.update(0, t); }
    CHECK(tap.flowing(), "pre-flow not confirmed");
    CycleGates g; g.enter(tap, drain);
    CHECK(!g.fill1Done(tap, drain, MIN_FILL), "Fill1 done without a drain");
    CHECK(g.tapSeen, "flow already running at Start not counted for Fill1");
    (void)tapEarly;
  });

  test("S6 soapy drain still running when Fill2 begins: waits for a new drain", [&] {
    // rinse fill starts while the soapy drain is still running; drain then stops and the rinse drain follows
    auto tapOverlap   = [](uint32_t t) { return (t < 120000 || (t >= 700000 && t < 800000)) ? 8.0f : 0.0f; };
    auto drainOverlap = [](uint32_t t) { return ((t >= 600000 && t < 720000) || (t >= 1500000 && t < 1620000)) ? 18.0f : 0.0f; };
    Result r = cycle(tapOverlap, drainOverlap, 2000000, 0);
    std::printf("    drain1 done at %d s, fill2 at %d s\n", r.drain1, r.fill2);
    CHECK(r.fill2 >= 1500 && r.fill2 <= 1506, "Fill2 ended on the old soapy drain (would save soapy water)");
  });

  test("S7 trickle of 1.5 L/min between drains: waits at the default threshold, advances with 3 L/min", [&] {
    auto drainSiphon = [&](uint32_t t) { float d = drainTypical(t); return d > 0 ? d : (t > 720000 ? 1.5f : 0.0f); };
    Result r1 = cycle(tapTypical, drainSiphon, 2000000, 0);
    Result r3 = cycle(tapTypical, drainSiphon, 2000000, 0, 3.0f);
    std::printf("    default 1 L/min: fill2 %d s; threshold 3 L/min: fill2 %d s\n", r1.fill2, r3.fill2);
    CHECK(r1.fill2 == -1, "a real 1.5 L/min flow above the threshold was ignored");
    CHECK(r3.fill2 >= 1500 && r3.fill2 <= 1506, "raising the flow threshold did not clear the trickle");
  });

  test("S8 60 Hz mains pickup on the drain wire looks like ~10 L/min (must be fixed in wiring)", [&] {
    FlowMeter m = makeMeter(DRAIN_PPL); Sim s(DRAIN_PPL);
    run(s, m, 10000, [](uint32_t) { return 0.0f; }, 60.0f);
    std::printf("    reads %.1f L/min with no water\n", m.rateLpm());
    CHECK(m.flowing() && m.rateLpm() > 9.0f, "expected the pickup to be visible as flow in the sensor check");
  });

  std::printf("\n\"Machine is filled\" guard (2.6): Fill 1/4 and Rinse 3/4 end only on a drain that starts after the fill\n\n");

  // START at 10 s; returns when Fill1 ended (s) for the new and the 2.5 logic.
  struct FillEnd { double now = -1, old = -1; };
  auto fillStep = [](std::function<float(uint32_t)> tapL, std::function<float(uint32_t)> drainL, float minL) {
    FlowMeter tap = makeMeter(TAP_PPL), drain = makeMeter(DRAIN_PPL);
    CycleGates g; OldGates og; FillEnd r; uint32_t tp = 0, dp = 0; double ta = 0, da = 0; bool entered = false;
    tap.update(0, 0); drain.update(0, 0);
    for (uint32_t t = 10; t < 900000; t += 10) {
      ta += tapL(t) / 60.0 * TAP_PPL * 0.01;     while (ta >= 1) { ta -= 1; ++tp; }
      da += drainL(t) / 60.0 * DRAIN_PPL * 0.01; while (da >= 1) { da -= 1; ++dp; }
      tap.update(tp, t); drain.update(dp, t);
      if (!entered && t >= 10000) { g.enter(tap, drain); og.enter(tap, drain); entered = true; }
      if (!entered || t % 250) continue;
      if (r.now < 0 && g.fill1Done(tap, drain, minL)) r.now = t / 1000.0;
      if (r.old < 0 && og.fill1Done(tap, drain))      r.old = t / 1000.0;
      if (r.now >= 0 && r.old >= 0) break;
    }
    return r;
  };
  auto washFill = [](uint32_t t) { return (t >= 25000 && t < 145000) ? 8.0f : 0.0f; };   // fill 25-145 s
  auto soapy    = [](uint32_t t) { return t >= 600000 && t < 720000; };                  // wash drain at 600 s

  test("S9 drain pump runs at program start (8-20 s), then the machine fills: Fill1 waits for the wash drain", [&] {
    FillEnd r = fillStep(washFill, [&](uint32_t t) { return ((t >= 8000 && t < 20000) || soapy(t)) ? 18.0f : 0.0f; }, MIN_FILL);
    std::printf("    Fill1 ended at %.1f s (firmware 2.5: %.1f s, 4 s after filling began)\n", r.now, r.old);
    CHECK(r.now >= 600 && r.now <= 606, "Fill1 did not wait for the wash drain");
    CHECK(r.old < 40, "model did not reproduce the 2.5 fault");
  });

  test("S10 6 s of interference on the drain wire while filling: ignored", [&] {
    FillEnd r = fillStep(washFill, [&](uint32_t t) { return ((t >= 30000 && t < 36000) || soapy(t)) ? 10.0f : 0.0f; }, MIN_FILL);
    std::printf("    Fill1 ended at %.1f s (firmware 2.5: %.1f s)\n", r.now, r.old);
    CHECK(r.now >= 600 && r.now <= 606, "noise during the fill ended Fill1");
  });

  test("S11 small splash (1 L) then a drain, real fill later: the splash does not count as filled", [&] {
    auto tapL = [](uint32_t t) { return ((t >= 12000 && t < 19500) || (t >= 60000 && t < 180000)) ? 8.0f : 0.0f; };
    FillEnd r = fillStep(tapL, [&](uint32_t t) { return ((t >= 30000 && t < 45000) || soapy(t)) ? 18.0f : 0.0f; }, MIN_FILL);
    std::printf("    Fill1 ended at %.1f s\n", r.now);
    CHECK(r.now >= 600 && r.now <= 606, "a 1 L splash was taken as the fill");
  });

  test("S12 machine tops up during the wash (two short refills): still ends on the wash drain", [&] {
    auto tapL = [](uint32_t t) { return ((t >= 25000 && t < 145000) || (t >= 300000 && t < 310000) || (t >= 450000 && t < 458000)) ? 8.0f : 0.0f; };
    FillEnd r = fillStep(tapL, [&](uint32_t t) { return soapy(t) ? 18.0f : 0.0f; }, MIN_FILL);
    CHECK(r.now >= 600 && r.now <= 606, "top-ups disturbed Fill1");
  });

  test("S13 inlet never closes (machine keeps taking water): Fill1 waits and the step watchdog alerts", [&] {
    FillEnd r = fillStep([](uint32_t t) { return t >= 25000 ? 8.0f : 0.0f; }, [&](uint32_t t) { return soapy(t) ? 18.0f : 0.0f; }, MIN_FILL);
    CHECK(r.now < 0, "Fill1 ended although the fill never finished");
  });

  std::printf("\nPump protection (PumpGuard): tank pump during the Fill 1/4 tank-water fill\n\n");

  // Physical model, 10 ms loop passes:
  //  - the machine draws water while draw(t) is true (its inlet valve is open);
  //  - water passes the tap sensor from the tap (tap mode) or from the pump
  //    (tank mode) at 8 L/min, but the pump delivers nothing for spinUpMs after
  //    it starts, and nothing at all if dry is set;
  //  - the firmware logic: PumpGuard::mustStop every pass, allow() every 250 ms.
  // useGuard = false reproduces firmware 2.4 (pump on while the slow meter reads flowing).
  struct PumpRun { int pumpStarts = 0; uint32_t deadHeadMs = 0, worstDeadHeadMs = 0, pumpOnMs = 0; uint32_t dry = 0; };
  auto pumpSim = [](std::function<bool(uint32_t)> draw, uint32_t endMs, uint32_t spinUpMs, bool dry, bool useGuard) {
    FlowMeter tap = makeMeter(TAP_PPL); PumpGuard g; g.configure(1000, 300);
    PumpRun r; bool tank = false; uint32_t pumpSince = 0, pulses = 0, lastPulseMs = 0, seen = 0, dh = 0; double acc = 0;
    for (uint32_t t = 0; t < endMs; t += 10) {
      bool d = draw(t);
      bool delivering = tank ? (!dry && t - pumpSince >= spinUpMs) : true;
      if (d && delivering) { acc += 8.0 / 60.0 * TAP_PPL * 0.010; while (acc >= 1) { acc -= 1; ++pulses; } }
      if (pulses != seen) { seen = pulses; lastPulseMs = t; }
      if (useGuard && tank && g.mustStop(lastPulseMs, tap.starts(), t)) tank = false;
      tap.update(pulses, t);
      if (t % 250 == 0) {
        bool want = tap.flowing();
        bool on = useGuard ? g.allow(want, tap.starts(), t) : want;
        if (on && !tank) { tank = true; pumpSince = t; ++r.pumpStarts; }
        if (!on) tank = false;
      }
      if (tank) { r.pumpOnMs += 10; if (!d) { r.deadHeadMs += 10; dh += 10; if (dh > r.worstDeadHeadMs) r.worstDeadHeadMs = dh; } else dh = 0; }
      else dh = 0;
    }
    r.dry = g.dryStarts();
    return r;
  };

  test("P1 machine closes its inlet: pump stops within 0.35 s (2.4 ran on for about 3.7 s)", [&] {
    auto draw = [](uint32_t t) { return t >= 1000 && t < 31000; };
    PumpRun now = pumpSim(draw, 40000, 400, false, true), old = pumpSim(draw, 40000, 400, false, false);
    std::printf("    pump against a closed inlet: firmware 2.5 %u ms, firmware 2.4 %u ms\n", now.worstDeadHeadMs, old.worstDeadHeadMs);
    CHECK(now.pumpStarts == 1, "pump did not run exactly once for one draw");
    CHECK(now.worstDeadHeadMs <= 350, "pump ran more than 0.35 s against the closed inlet");
    CHECK(old.worstDeadHeadMs >= 1500, "model did not reproduce the 2.4 run-on");
  });

  test("P2 pump spin-up gap of 0.6 s is not mistaken for the inlet closing", [&] {
    PumpRun r = pumpSim([](uint32_t t) { return t >= 1000 && t < 31000; }, 40000, 600, false, true);
    CHECK(r.pumpStarts == 1 && r.pumpOnMs > 20000, "pump tripped during spin-up");
  });

  test("P3 dry pump (no water delivered): stops within 1.3 s, tap takes over, no retry on the same draw", [&] {
    PumpRun r = pumpSim([](uint32_t t) { return t >= 1000 && t < 31000; }, 40000, 0, true, true);
    std::printf("    pump starts %d, pump on %u ms, dry starts %u\n", r.pumpStarts, r.pumpOnMs, r.dry);
    CHECK(r.pumpStarts == 1, "pump retried on the same draw");
    CHECK(r.pumpOnMs <= 1300, "dry pump ran more than 1.3 s");
    CHECK(r.dry == 1, "dry start not counted");
  });

  test("P4 machine draws in bursts (8 s on, 4 s off): pump follows each draw, never runs on", [&] {
    auto bursts = [](uint32_t t) { return t >= 1000 && t < 61000 && ((t - 1000) % 12000) < 8000; };
    PumpRun now = pumpSim(bursts, 70000, 400, false, true), old = pumpSim(bursts, 70000, 400, false, false);
    std::printf("    5 draws: pump starts %d; dead-head total %u ms (2.4: %u ms)\n", now.pumpStarts, now.deadHeadMs, old.deadHeadMs);
    CHECK(now.pumpStarts == 5, "pump did not start once per draw");
    CHECK(now.worstDeadHeadMs <= 350, "pump ran on after a draw ended");
  });

  test("P5 short pause (1 s) between draws: pump stays off for the next draw, tap supplies it", [&] {
    auto twoDraws = [](uint32_t t) { return (t >= 1000 && t < 11000) || (t >= 12000 && t < 22000); };
    PumpRun r = pumpSim(twoDraws, 30000, 400, false, true);
    std::printf("    pump starts %d, worst dead-head %u ms\n", r.pumpStarts, r.worstDeadHeadMs);
    CHECK(r.worstDeadHeadMs <= 350, "pump ran on during the short pause");
    CHECK(r.pumpStarts == 1, "pump restarted without a new confirmed draw");
  });

  std::printf("\n%d checks passed, %d failed\n", passes, failures);
  return failures == 0 ? 0 : 1;
}
