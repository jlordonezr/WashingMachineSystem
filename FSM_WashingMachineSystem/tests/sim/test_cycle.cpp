// Full-cycle simulation: runs the REAL firmware (sketch, GreyWaterSystem.cpp,
// GreyWaterSettings.cpp, FlowMeter.h, AgileStateMachine) against a simulated
// washing machine, tank, flow sensors, level switches and buttons.
// Usage: test_cycle <scenario> [flash-file]     (see run_sim.sh)
#include "GreyWaterSystem.h"
#include "GreyWaterNet.h"
#include <Preferences.h>
#include <esp_system.h>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>

void setup();
void loop();
SerialStub Serial;
int oledAddress = 0x3C;      // the screen answers here (0 = no screen)
TwoWire Wire;

// ── simulated hardware ──
static uint64_t simUs = 0;
static int pinOut[64], pinMode_[64];
static void (*isr[64])() = {nullptr};
static esp_reset_reason_t simReset = ESP_RST_POWERON;
uint32_t millis() { return (uint32_t)(simUs / 1000ULL); }
uint32_t micros() { return (uint32_t)simUs; }
void delay(uint32_t ms) { simUs += (uint64_t)ms * 1000ULL; }
void pinMode(int p, int m) { pinMode_[p] = m; }
void digitalWrite(int p, int v) { pinOut[p] = v; }
void attachInterrupt(int p, void (*fn)(), int) { isr[p] = fn; }
esp_reset_reason_t esp_reset_reason() { return simReset; }

static double tankL = 30.0;                 // litres in the storage tank (low switch 10 L, high switch 40 L)
static std::vector<std::pair<uint64_t,uint64_t>> startPresses;   // START pressed in [from, to) µs (3.17: bounce can be simulated)
static uint64_t stopHeldUntil = 0;
static bool startIsDown() { for (auto& p : startPresses) if (simUs >= p.first && simUs < p.second) return true; return false; }
static bool lowStuckDry = false;            // F7: the low float has failed and always reads dry
static bool highBobs = false;               // F8: between 38 and 41 L the high float bobs every 300 ms
int digitalRead(int p) {
  if (p == IN_LOWLEVEL)  return (!lowStuckDry && tankL > 10.0) ? HIGH : LOW;
  if (p == IN_HIGHLEVEL) {
    if (highBobs && tankL > 38.0 && tankL < 41.0) return ((simUs / 300000ULL) % 2) ? HIGH : LOW;
    return tankL > 40.0 ? HIGH : LOW;
  }
  if (p == START_BUTTON) return startIsDown() ? LOW : HIGH;
  if (p == STOP_BUTTON)  return simUs < stopHeldUntil ? LOW : HIGH;
  return LOW;
}

// ── safety task stand-ins (GreyWaterTask.cpp on the board) ──
// The simulation is single-threaded: stepWorld() calls safetyTick() every 20 ms
// of simulated time, also while loop() is "stalled" (F10, F11).
void gwLock() {}
void gwUnlock() {}
void safetyBegin() {}
static uint64_t nextSafetyUs = 0, loopStalledUntil = 0;

// ── network stub: records alerts ──
struct Alert { std::string title; int prio; };
static std::vector<Alert> alerts;
void netBegin() {}
void netLoop() {}
void netStartConfigPortal() {}
bool netPortalActive() { return false; }
bool netIsConnected() { return false; }
void notify(const char* title, const char* body, uint8_t prio, const char*) { (void)body; alerts.push_back({title, prio}); logEvent("Alert: %s", title); }

// ── washing machine program (seconds after START) ──
struct DrainRate { double from, to, lpm; };   // drain flow other than 18 L/min in [from, to) (F13)
struct Program { std::vector<std::pair<double,double>> draws, drains; std::vector<DrainRate> rates; };
static Program prog;
static double startAt = -1;
static bool in(const std::vector<std::pair<double,double>>& w, double t) { for (auto& p : w) if (t >= p.first && t < p.second) return true; return false; }

static int failures = 0, passes = 0;
#define CHECK(c, ...) do { if (c) ++passes; else { ++failures; printf("    FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Recording
struct Rec { std::vector<std::pair<int,double>> states; std::vector<std::string> msgs; uint32_t worstDeadHeadMs = 0, deadHead = 0;
             double pumpOnFirst = -1; bool savedWhileSaving = false; bool pumpWhileTap = false;
             bool pumpInState[8] = {}; bool fillEverOn = false;   // 3.18: pump on in each state; fill valve ever on
             int fillSwitches = 0; bool fillWhileHighWet = false, washWhileHighWet = false; uint32_t fillOnAfterHighMs = 0, highWetMs = 0; };
static Rec rec;
static double pumpSince = -1, tapAcc = 0, drainAcc = 0;

static double now() { return simUs / 1e6; }
static double progT() { return startAt < 0 ? -1 : now() - startAt; }

static void stepWorld(uint32_t dtMs) {
  double t = progT();
  bool draw  = t >= 0 && in(prog.draws, t);
  bool drain = t >= 0 && in(prog.drains, t);
  bool tapOpen = pinOut[OUT_TAP_EV] == HIGH, pumpOn = pinOut[OUT_MOTOR] == HIGH;
  if (pumpOn && pumpSince < 0) pumpSince = now();
  if (!pumpOn) pumpSince = -1;
  bool pumpDelivers = pumpOn && pumpSince >= 0 && now() - pumpSince >= 0.4 && tankL > 0.5;
  double tapFlow = (draw && (tapOpen || pumpDelivers)) ? 8.0 : 0.0;
  if (draw && pumpDelivers && !tapOpen) tankL -= 8.0 * dtMs / 60000.0;
  double drainFlow = drain ? 18.0 : 0.0;
  for (auto& r : prog.rates) if (t >= r.from && t < r.to) drainFlow = r.lpm;
  if (drainFlow > 0 && pinOut[OUT_FILL_EV] == HIGH && pinOut[OUT_DRAIN_EV] == LOW) { tankL = std::min(45.0, tankL + drainFlow * dtMs / 60000.0); rec.savedWhileSaving = true; }
  if (pinOut[OUT_WASH_EV] == HIGH) tankL = std::min(45.0, tankL + 6.0 * dtMs / 60000.0);   // tank-wash water (3.13: 6 L/min, as the firmware assumes)
  if (pumpOn && tapOpen) rec.pumpWhileTap = true;
  if (pumpOn && activeState >= 0 && activeState < 8) rec.pumpInState[activeState] = true;
  if (pinOut[OUT_FILL_EV] == HIGH) rec.fillEverOn = true;
  static int lastFill = LOW;
  if (pinOut[OUT_FILL_EV] != lastFill) { lastFill = pinOut[OUT_FILL_EV]; rec.fillSwitches++; }
  // Interlock checks allow the debounce time (0.2 s) plus one 250 ms pass after the high float is wet.
  if (digitalRead(IN_HIGHLEVEL) == HIGH) rec.highWetMs += dtMs; else rec.highWetMs = 0;
  if (rec.highWetMs > 500 && pinOut[OUT_FILL_EV] == HIGH) rec.fillWhileHighWet = true;
  if (rec.highWetMs > 500 && pinOut[OUT_WASH_EV] == HIGH) rec.washWhileHighWet = true;
  if (pumpOn && rec.pumpOnFirst < 0) rec.pumpOnFirst = t;
  if (pumpOn && !draw) { rec.deadHead += dtMs; rec.worstDeadHeadMs = std::max(rec.worstDeadHeadMs, rec.deadHead); } else rec.deadHead = 0;
  tapAcc   += tapFlow   / 60.0 * TAP_PULSES_PER_LITRE   * dtMs / 1000.0;
  drainAcc += drainFlow / 60.0 * DRAIN_PULSES_PER_LITRE * dtMs / 1000.0;
  simUs += (uint64_t)dtMs * 1000ULL;
  if (tapAcc >= 1)   { tapAcc -= 1;   if (isr[TAP_PIN])   isr[TAP_PIN](); }
  if (drainAcc >= 1) { drainAcc -= 1; if (isr[DRAIN_PIN]) isr[DRAIN_PIN](); }
  if (simUs >= nextSafetyUs) { nextSafetyUs = simUs + SAFETY_TICK_MS * 1000ULL; safetyTick(); }
  if (simUs < loopStalledUntil) return;     // loop() blocked, e.g. by a slow web request
  int before = activeState;
  loop();
  if (activeState != before) rec.states.push_back({activeState, progT()});
  std::string m = displayGetMessage();
  if (rec.msgs.empty() || rec.msgs.back() != m) rec.msgs.push_back(m);
}
static void runFor(double seconds) { uint64_t end = simUs + (uint64_t)(seconds * 1e6); while (simUs < end) stepWorld(2); }
static void holdStartFor(double s) { startPresses.push_back({simUs, simUs + (uint64_t)(s * 1e6)}); }
// START at Ready (3.17: the cycle starts on the press); the washer program starts with it.
static void pressStart() { holdStartFor(0.15); startAt = now(); }
static void pressStop()  { stopHeldUntil  = simUs + 150000; }
// START during a cycle (3.17: switches the mode in steps 1-3); the washer program goes on.
static void tapStart() { holdStartFor(0.15); }
// Runs until the cycle is in this state (or the time limit passes).
static void runUntilState(int st, double maxS = 2000) { double end = now() + maxS; while (activeState != st && now() < end) runFor(0.05); }
static bool onScreen(const char* t) { for (auto& x : oled.shown) if (x == t) return true; return false; }
// Row (cursor y) a text on the screen was printed at, -1 if it is not there.
static int rowOf(const char* t) { for (size_t i = 0; i < oled.shown.size(); ++i) if (oled.shown[i] == t) return oled.shownY[i]; return -1; }
static double stateAt(int st) { for (auto& s : rec.states) if (s.first == st) return s.second; return -1; }
// True if any screen message had m as one of its pages ("a|b|c").
static bool sawMsg(const char* m) {
  for (auto& msg : rec.msgs) { std::string all = "|" + msg + "|"; if (all.find("|" + std::string(m) + "|") != std::string::npos) return true; }
  return false;
}
static bool eventSeen(const char* needle) { for (int i = 0; i < eventCount(); ++i) { uint32_t e, u; const char* tx; eventAt(i, e, u, tx); if (strstr(tx, needle)) return true; } return false; }
static int eventCountOf(const char* needle) { int n = 0; for (int i = 0; i < eventCount(); ++i) { uint32_t e, u; const char* tx; eventAt(i, e, u, tx); if (strstr(tx, needle)) ++n; } return n; }
static bool noSaveNow() { SystemStatus s; getSystemStatus(s); return s.noSave; }
static int modeNow() { SystemStatus s; getSystemStatus(s); return s.mode; }
static bool alertSeen(const char* t, int* prio = nullptr) { for (auto& a : alerts) if (a.title == t) { if (prio) *prio = a.prio; return true; } return false; }

// Scenarios run in English (their checks use the English texts) unless they ask
// for Spanish, the factory default from 3.1: settings are stored before setup().
static int simLanguage = LANG_EN;
static void boot() {
  simUs += 1000000;
  Preferences p;
  if (p.getBytesLength("cfg") == 0) { Settings s; settingsDefaults(s); s.language = simLanguage; p.putBytes("cfg", &s, sizeof(s)); }
  setup(); cfg.drain2DurationMs = 120000; runFor(1);   // step 4 shortened to 2 min
}
static void typicalProgram() { prog.draws = {{15,135},{300,308},{780,880}}; prog.drains = {{600,720},{1500,1620}}; }

int main(int argc, char** argv) {
  std::string sc = argc > 1 ? argv[1] : "F1";
  const char* flash = argc > 2 ? argv[2] : "/tmp/gw_sim_flash.bin";

  if (sc == "F1") {
    printf("F1 full cycle reusing tank water\n");
    typicalProgram(); boot(); pressStart(); runFor(5);
    CHECK(systemBusy(), "not busy during a cycle");
    runFor(1800);
    CHECK(rec.states.size() >= 6, "only %zu state changes", rec.states.size());
    double f1 = stateAt(stFill1), d1 = stateAt(stDrain1), f2 = stateAt(stFill2), d2 = stateAt(stDrain2);
    printf("    steps began at: wash %.1f s, soapy drain %.1f s, rinse %.1f s, save %.1f s\n", f1, d1, f2, d2);
    CHECK(f1 >= 0 && f1 < 1, "wash step began at %.1f s", f1);
    CHECK(d1 >= 600 && d1 <= 607, "soapy drain began at %.1f s (wash drain starts at 600 s)", d1);
    CHECK(f2 >= 780 && f2 <= 787, "rinse began at %.1f s (rinse fill starts at 780 s)", f2);
    CHECK(d2 >= 1500 && d2 <= 1507, "save began at %.1f s (rinse drain starts at 1500 s)", d2);
    CHECK(activeState == stIdle, "cycle did not end; state %d", activeState);
    CHECK(!systemBusy(), "still busy after the cycle");
    printf("    pump first on at %.1f s; worst run against a closed inlet %u ms\n", rec.pumpOnFirst, rec.worstDeadHeadMs);
    CHECK(rec.pumpOnFirst >= 19 && rec.pumpOnFirst <= 20.5, "pump started at %.1f s (draw confirmed at about 19 s)", rec.pumpOnFirst);
    CHECK(rec.worstDeadHeadMs <= 350, "pump ran %u ms against the closed inlet", rec.worstDeadHeadMs);
    CHECK(!rec.pumpWhileTap, "pump and tap valve were on together");
    CHECK(rec.savedWhileSaving, "rinse water was never saved to the tank");
    const char* expected[] = {"Waiting to fill", "Reusing water", "Washing...", "Soapy water", "Rinsing...", "Saving water"};
    for (const char* m : expected) CHECK(sawMsg(m), "screen never showed \"%s\"", m);
    CHECK(stats.totalCycles == 1 && stats.totalReusedL > 10 && stats.totalSavedL > 10, "stats: cycles %u, reused %.1f, saved %.1f",
          (unsigned)stats.totalCycles, stats.totalReusedL, stats.totalSavedL);
    printf("    reused %.1f L, saved %.1f L; typical step times %u / %u / %u s\n", stats.totalReusedL, stats.totalSavedL,
           (unsigned)stats.stepAvgS[0], (unsigned)stats.stepAvgS[1], (unsigned)stats.stepAvgS[2]);
    CHECK(stats.stepSamples[0] == 1 && stats.stepAvgS[0] >= 600 && stats.stepAvgS[0] <= 608, "step 1 typical time %u s", (unsigned)stats.stepAvgS[0]);
    int prio = 0;
    CHECK(alertSeen("Reuse Water: cycle complete", &prio) && prio == 2, "cycle-complete alert missing or not low priority (%d)", prio);
    for (const char* e : {"Cycle started", "Step 1/4 wash started", "Machine filled", "Pump on", "Pump stopped: machine closed its inlet", "Cycle complete"})
      CHECK(eventSeen(e), "event log has no \"%s\"", e);
    Preferences p; CHECK(p.getBytesLength("cyc") == 0, "cycle marker left behind after a normal end");
    // Every step has been timed once, so the next cycle can say how long it still needs.
    rec.msgs.clear(); pressStart(); runFor(30);
    bool timeLeft = false; for (auto& m : rec.msgs) if (m.find(" min left") != std::string::npos) timeLeft = true;
    CHECK(timeLeft, "second cycle never showed \"... min left\"");
  }
  else if (sc == "F2") {
    printf("F2 machine runs its drain pump at program start, then fills\n");
    typicalProgram(); prog.drains.insert(prog.drains.begin(), {0, 12}); boot(); pressStart(); runFor(700);
    double d1 = stateAt(stDrain1);
    printf("    soapy drain began at %.1f s\n", d1);
    CHECK(d1 >= 600 && d1 <= 607, "wash step ended early at %.1f s", d1);
  }
  else if (sc == "F3") {
    printf("F3 STOP pressed during the wash\n");
    typicalProgram(); boot(); pressStart(); runFor(300); pressStop(); runFor(3);
    CHECK(activeState == stIdle, "not back to Ready after STOP");
    CHECK(pinOut[OUT_MOTOR] == LOW && pinOut[OUT_FILL_EV] == LOW && pinOut[OUT_WASH_EV] == LOW, "pump, fill or wash output still on");
    CHECK(pinOut[OUT_TAP_EV] == HIGH && pinOut[OUT_DRAIN_EV] == HIGH, "tap and drain not back to normal");
    int prio = 0; CHECK(alertSeen("Reuse Water: cycle stopped", &prio) && prio == 1, "stop alert missing or not minimum priority");
    Preferences p; CHECK(p.getBytesLength("cyc") == 0, "cycle marker left behind after STOP");
  }
  else if (sc == "F4a") {
    printf("F4 power cut during the rinse (part 1: run until 900 s, then cut power)\n");
    typicalProgram(); boot(); pressStart(); runFor(900);
    CHECK(activeState == stFill2, "expected to be rinsing, state %d", activeState);
    Preferences p; CHECK(p.getBytesLength("cyc") > 0, "no cycle marker in flash during the cycle");
    Preferences::saveFile(flash);
  }
  else if (sc == "F4b") {
    printf("F4 power cut during the rinse (part 2: power back)\n");
    Preferences::loadFile(flash); simReset = ESP_RST_POWERON; boot(); runFor(11);  // after the start-up animation (7.4 s) and "System ready"
    int st; uint32_t ep; bool intr = interruptedInfo(st, ep);
    CHECK(intr && st == stFill2, "interruption not detected (flag %d, step %d)", intr, st);
    CHECK(activeState == stIdle && pinOut[OUT_MOTOR] == LOW, "not safely in Ready after power-up");
    CHECK(alertSeen("Reuse Water: cycle interrupted"), "no interrupted alert");
    CHECK(eventSeen("Cycle interrupted"), "interruption not in the event log");
    bool shown = false; for (auto& s : oled.shown) if (s == "INTERRUPTED") shown = true;
    CHECK(shown, "screen does not show INTERRUPTED");
    Preferences p; CHECK(p.getBytesLength("cyc") == 0, "marker not cleared, the alert would repeat at every power-up");
    CHECK(stats.totalReusedL == 0, "unexpected history in a fresh simulation");
    pressStart(); runFor(2); CHECK(!interruptedInfo(st, ep), "notice not cleared by a new Start");
  }
  else if (sc == "F5") {
    printf("F5 normal power-up with no cycle running\n");
    boot(); int st; uint32_t ep;
    CHECK(!interruptedInfo(st, ep) && alerts.empty(), "false interruption alert at a normal power-up");
    CHECK(eventSeen("Controller started (Switched on or power cut)"), "start-up not in the event log");
    CHECK(!strcmp(cfg.ntfyServer, "https://ntfy.hostux.net"), "factory default alert server is %s, not https://ntfy.hostux.net (3.14)", cfg.ntfyServer);
    CHECK(cfg.screenFont == 0, "factory default screen font is %d, not Hyperlegible (0)", cfg.screenFont);
    // Every screen font from Settings redraws the Ready screen at once, with the same text (3.4).
    runFor(11);                                            // past the start-up animation (7.4 s) and notice
    auto shownText = []() { return oled.shown.empty() ? std::string() : oled.shown.back(); };
    std::string before = shownText();
    for (int f = 1; f < SCREEN_FONT_COUNT; ++f) {
      cfg.screenFont = f; oled.shown.clear(); runFor(0.1);
      CHECK(!oled.shown.empty() && shownText() == before, "screen font %d: screen not redrawn with the same text", f);
    }
    cfg.screenFont = 0;
  }
  else if (sc == "F7") {
    printf("F7 low float failed dry, tank full (high float wet)\n");
    tankL = 42.0; lowStuckDry = true; typicalProgram(); boot(); runFor(3);
    CHECK(tankFault(), "disagreement not detected");
    CHECK(alertSeen("Reuse Water: check level switches"), "no level-switch alert");
    pressStart(); runFor(1800);
    CHECK(rec.pumpOnFirst < 0, "pump ran with the low float dry");
    CHECK(!rec.fillWhileHighWet, "fill valve open into a tank whose high float is wet (overflow)");
    CHECK(!rec.washWhileHighWet, "tank-wash valve open while the high float is wet");
    CHECK(stateAt(stDrain2) > 0 && activeState == stIdle, "cycle did not complete");
    CHECK(tankL <= 42.01, "tank grew to %.1f L", tankL);
  }
  else if (sc == "F8") {
    printf("F8 high float bobbing at the mark during the save step\n");
    tankL = 30.0; highBobs = true; typicalProgram(); boot(); pressStart(); runFor(1800);
    printf("    fill valve switched %d times; tank %.1f L\n", rec.fillSwitches, tankL);
    CHECK(rec.fillSwitches <= 2, "fill valve switched %d times (chatter)", rec.fillSwitches);
    CHECK(rec.savedWhileSaving, "no water saved before the tank filled");
    CHECK(tankL < 39.0, "tank kept filling to %.1f L while the float bobbed", tankL);
    CHECK(activeState == stIdle, "cycle did not complete");
  }
  else if (sc == "F9") {
    printf("F9 screen missing at start-up\n");
    oledAddress = 0; typicalProgram(); boot();
    CHECK(!displayAvailable(), "missing screen not detected");
    CHECK(alertSeen("Reuse Water: screen not working"), "no screen alert");
    CHECK(eventSeen("Screen not found"), "missing screen not in the event log");
    pressStart(); runFor(1800);
    CHECK(stats.totalCycles == 1, "cycle did not complete without a screen");
  }
  else if (sc == "F10") {
    printf("F10 loop() blocked for 3 s just as the machine closes its inlet (pump running)\n");
    typicalProgram(); boot(); pressStart(); runFor(134.9);
    CHECK(pinOut[OUT_MOTOR] == HIGH, "pump not running before the stall");
    loopStalledUntil = simUs + 3000000ULL;      // the first draw ends at 135 s
    runFor(3.5);
    printf("    worst run against a closed inlet %u ms (2.9 had no protection during a stall)\n", rec.worstDeadHeadMs);
    CHECK(rec.worstDeadHeadMs <= 350, "pump ran %u ms against the closed inlet while loop() was blocked", rec.worstDeadHeadMs);
    CHECK(!rec.pumpWhileTap, "pump and tap valve on together");
    runFor(1800);
    CHECK(activeState == stIdle && stats.totalCycles == 1, "cycle did not complete after the stall");
    CHECK(eventSeen("Pump stopped: machine closed its inlet"), "pump stop not logged after the stall");
  }
  else if (sc == "F11") {
    printf("F11 loop() blocked for 3 s while the tank fills during the save step\n");
    typicalProgram(); boot(); pressStart(); runFor(1505);   // step 4 begins at about 1504.5 s
    tankL = 39.6;                                             // 0.4 L below the high float
    CHECK(activeState == stDrain2 && pinOut[OUT_FILL_EV] == HIGH, "not saving at the start of step 4");
    loopStalledUntil = simUs + 3000000ULL;      // tank passes 40 L about 1.3 s later
    runFor(3.5);
    printf("    tank %.2f L after the stall\n", tankL);
    CHECK(!rec.fillWhileHighWet, "fill valve stayed open into a full tank while loop() was blocked");
    CHECK(tankL < 40.4, "tank overfilled to %.2f L", tankL);
    runFor(200);
    CHECK(activeState == stIdle, "cycle did not complete");
  }
  else if (sc == "F13") {
    printf("F13 step 4: a trickle below the minimum save flow goes to the drain, a new pump-out is saved again (3.4)\n");
    tankL = 5.0; typicalProgram(); boot(); cfg.drain2DurationMs = 600000;        // step 4 lasts 10 min here
    // Rinse pump-out from 1500 s at 18 L/min, 0.3 L/min from 1560 s, a spin pump-out 1640-1670 s at 12 L/min, then 0.3 L/min.
    prog.drains = {{600,720},{1500,1800}};
    prog.rates = {{1560,1640,0.3},{1640,1670,12.0},{1670,1800,0.3}};
    auto saving   = []() { return pinOut[OUT_FILL_EV] == HIGH && pinOut[OUT_DRAIN_EV] == LOW; };
    auto draining = []() { return pinOut[OUT_FILL_EV] == LOW && pinOut[OUT_DRAIN_EV] == HIGH; };
    pressStart(); runFor(1540);
    CHECK(activeState == stDrain2 && saving(), "not saving during the 18 L/min pump-out (state %d)", activeState);
    double tLow = -1; while (progT() < 1580 && tLow < 0) { runFor(0.05); if (draining()) tLow = progT(); }
    printf("    trickle starts at 1560 s; fill closed and drain open at %.2f s\n", tLow);
    CHECK(tLow >= 1560 && tLow <= 1563, "fill valve not closed / drain not opened within 3 s of the trickle (%.2f s)", tLow);
    double savedAtLow = tankL; runFor(1640 - progT());
    CHECK(draining() && tankL == savedAtLow, "the 0.3 L/min trickle went to the tank");
    double tBack = -1; while (progT() < 1660 && tBack < 0) { runFor(0.05); if (saving()) tBack = progT(); }
    printf("    spin pump-out at 1640 s; saving again at %.2f s\n", tBack);
    CHECK(tBack >= 1642 && tBack <= 1646, "saving did not resume 2-6 s into the new pump-out (%.2f s)", tBack);
    runFor(1669 - progT());
    CHECK(saving(), "stopped saving during the 12 L/min pump-out");
    double tLow2 = -1; while (progT() < 1680 && tLow2 < 0) { runFor(0.05); if (draining()) tLow2 = progT(); }
    CHECK(tLow2 >= 1670 && tLow2 <= 1673, "second trickle not sent to the drain (%.2f s)", tLow2);
    CHECK(eventSeen("Saving paused") && eventSeen("Saving resumed"), "pause / resume missing from the event log");
    CHECK(sawMsg("Low flow"), "screen never showed \"Low flow\"");
    CHECK(!alertSeen("Reuse Water: tank full") && !tankFull(), "a low flow raised the tank-full alert");
    printf("    tank %.1f L; fill valve switched %d times\n", tankL, rec.fillSwitches);
  }
  else if (sc == "F14") {
    printf("F14 Reuse only from the app: tank water in step 1, rinse water to the drain in step 4 (3.7)\n");
    typicalProgram(); boot();
    cfg.allowRemoteStart = false;
    CHECK(!requestRemoteStart(cmReuseOnly), "Reuse only accepted with starting from the app turned off");
    cfg.allowRemoteStart = true;
    CHECK(requestRemoteStart(cmReuseOnly), "Reuse only refused at Ready");
    startAt = now();                                   // the washer program starts with the app start
    runFor(5);
    CHECK(systemBusy() && eventSeen("Cycle started: reuse only"), "Reuse only cycle did not start");
    CHECK(!requestRemoteStart(cmReuseSave), "a second start accepted while a cycle runs");
    SystemStatus snap; getSystemStatus(snap);
    CHECK(snap.noSave, "status does not report the Reuse only cycle");
    bool fillInStep4 = false, drainShut = false, header = false;
    while (activeState != stIdle && now() < 2200) {
      runFor(0.25);
      if (activeState == stDrain2) {
        if (pinOut[OUT_FILL_EV] == HIGH) fillInStep4 = true;
        if (pinOut[OUT_DRAIN_EV] == LOW) drainShut = true;
        if (!strcmp(displayGetTitle(), "DRAIN 4 OF 4")) header = true;
      }
    }
    CHECK(stateAt(stDrain2) >= 1500, "step 4 never began");
    CHECK(!fillInStep4 && !rec.savedWhileSaving, "rinse water went to the tank in a Reuse only cycle");
    CHECK(!drainShut, "drain valve closed during step 4 of a Reuse only cycle");
    CHECK(activeState == stIdle && stats.totalCycles == 1, "Reuse only cycle did not complete");
    CHECK(stats.totalReusedL > 10 && stats.totalSavedL == 0, "stats: reused %.1f L, saved %.1f L (expected reused, nothing saved)",
          stats.totalReusedL, stats.totalSavedL);
    CHECK(sawMsg("Getting ready...|Reuse only") || sawMsg("Reuse only"), "screen never showed \"Reuse only\" at the start");
    CHECK(sawMsg("Not saving"), "step 4 screen never showed \"Not saving\"");
    CHECK(header, "step 4 header is not DRAIN 4 OF 4");
    CHECK(!alertSeen("Reuse Water: tank full"), "tank-full alert in a Reuse only cycle");
    CHECK(eventSeen("Step 4/4 final drain started (reuse only"), "event log has no reuse-only step 4");
    getSystemStatus(snap); CHECK(!snap.noSave, "Reuse only still reported after the cycle");
    printf("    reused %.1f L, saved %.1f L\n", stats.totalReusedL, stats.totalSavedL);
    // The START button on the controller is always a normal cycle, and saves again.
    tankL = 20.0; rec.savedWhileSaving = false;
    pressStart(); runFor(1800);
    CHECK(rec.savedWhileSaving && stats.totalSavedL > 1, "START button after a Reuse only cycle did not save (saved %.1f L)", stats.totalSavedL);
  }
  else if (sc == "F15") {
    printf("F15 START on the panel: one press starts Reuse and save at once, then step 4 saves (3.17)\n");
    typicalProgram(); boot();                             // the start-up animation is still running
    pressStart(); runFor(0.1);                            // START is still held
    CHECK(activeState != stIdle, "a press during the start-up animation did not start the cycle at once");
    pressStop(); runFor(6);
    CHECK(activeState == stIdle, "STOP did not end the cycle");
    pressStart(); runFor(0.1);                            // START is still held
    CHECK(activeState != stIdle && !noSaveNow(), "one press did not start Reuse and save while START was held (state %d, noSave %d)", activeState, noSaveNow());
    CHECK(onScreen("REUSE & SAVE") && onScreen("Press START") && onScreen("to change"), "start notice is not REUSE & SAVE / Press START / to change");
    CHECK(eventCountOf("Cycle started") == 2 && eventCountOf("Mode changed") == 0, "event log: %d starts, %d mode changes", eventCountOf("Cycle started"), eventCountOf("Mode changed"));
    runFor(4);
    CHECK(!onScreen("Press START") && onScreen("WASH 1 OF 4"), "the step screen did not come back after the notice");
    runFor(1800);
    CHECK(stateAt(stDrain2) > 0 && activeState == stIdle, "cycle did not run through step 4");
    CHECK(rec.savedWhileSaving && stats.totalSavedL > 1, "step 4 did not save to the tank (saved %.1f L)", stats.totalSavedL);
    CHECK(sawMsg("Saving water"), "step 4 screen never showed \"Saving water\"");
  }
  else if (sc == "F22") {
    printf("F22 a 2nd press in step 1 switches to Reuse only; step 4 sends the rinse to the drain (3.17)\n");
    typicalProgram(); boot(); runFor(11); pressStart(); runFor(60);
    CHECK(activeState == stFill1, "expected step 1, state %d", activeState);
    tapStart(); runFor(0.1);
    CHECK(noSaveNow(), "the 2nd press did not switch to Reuse only");
    CHECK(onScreen("REUSE ONLY") && onScreen("Press START") && onScreen("to change"), "notice is not REUSE ONLY / Press START / to change");
    CHECK(eventSeen("Mode changed: reuse only (panel)"), "the switch is not in the event log");
    { CycleSnapshot c; Preferences p; p.getBytes("cyc", &c, sizeof(c)); CHECK(c.mode == cmReuseOnly, "snapshot in flash not updated with the new mode"); }
    runFor(4);
    CHECK(!onScreen("REUSE ONLY") && onScreen("WASH 1 OF 4"), "the step screen did not come back after the notice");
    runFor(1800);
    CHECK(stateAt(stDrain2) > 0 && activeState == stIdle, "cycle did not run through step 4");
    CHECK(sawMsg("Not saving") && eventSeen("Step 4/4 final drain started (reuse only"), "step 4 did not run as Reuse only");
    CHECK(!rec.savedWhileSaving && stats.totalSavedL == 0, "rinse water saved in a cycle switched to Reuse only (%.1f L)", stats.totalSavedL);
    CHECK(!noSaveNow(), "Reuse only still reported after the cycle");
  }
  else if (sc == "F23") {
    printf("F23 press in step 1 (Reuse only), 3rd press in step 3 (Reuse all), 4th: back to Reuse and save; step 4 saves (3.17, 3.18)\n");
    typicalProgram(); boot(); runFor(11); pressStart(); runFor(60);
    tapStart(); runFor(1.5);
    CHECK(noSaveNow() && modeNow() == cmReuseOnly, "the 2nd press did not switch to Reuse only");
    runUntilState(stFill2); runFor(30);
    CHECK(activeState == stFill2, "expected step 3, state %d", activeState);
    tapStart(); runFor(0.1);
    CHECK(modeNow() == cmReuseAll && onScreen("REUSE ALL"), "the 3rd press did not switch to Reuse all (mode %d)", modeNow());
    runFor(1.5); tapStart(); runFor(0.1);
    CHECK(!noSaveNow() && modeNow() == cmReuseSave, "the 4th press did not switch back to Reuse and save");
    CHECK(onScreen("REUSE & SAVE") && onScreen("to change"), "notice is not REUSE & SAVE / to change");
    CHECK(eventSeen("Mode changed: reuse and save (panel)") && eventCountOf("Mode changed") == 3, "event log: %d mode changes", eventCountOf("Mode changed"));
    { CycleSnapshot c; Preferences p; p.getBytes("cyc", &c, sizeof(c)); CHECK(c.mode == cmReuseSave, "snapshot in flash not Reuse and save (%d)", c.mode); }
    runFor(1800);
    CHECK(rec.savedWhileSaving && stats.totalSavedL > 1, "step 4 did not save after switching back (saved %.1f L)", stats.totalSavedL);
  }
  else if (sc == "F24") {
    printf("F24 a press in step 4 does not change the mode; \"Mode fixed\" shown (3.17)\n");
    typicalProgram(); boot(); runFor(11); pressStart(); runUntilState(stDrain2); runFor(10);
    CHECK(activeState == stDrain2, "expected step 4, state %d", activeState);
    tapStart(); runFor(0.1);
    CHECK(activeState == stDrain2 && !noSaveNow(), "a press in step 4 changed the mode or the step");
    CHECK(onScreen("REUSE & SAVE") && onScreen("Mode fixed") && onScreen("Step 4 running"), "notice is not REUSE & SAVE / Mode fixed / Step 4 running");
    CHECK(eventCountOf("Mode changed") == 0, "a press in step 4 logged a mode change");
    runFor(4);
    CHECK(!onScreen("Mode fixed") && onScreen("SAVE 4 OF 4"), "the step 4 screen did not come back after the notice");
    runFor(300);
    CHECK(rec.savedWhileSaving && stats.totalSavedL > 1, "step 4 stopped saving after the press (saved %.1f L)", stats.totalSavedL);
  }
  else if (sc == "F25") {
    printf("F25 bounce: two press edges 50 ms apart at Ready start the cycle once (3.17)\n");
    typicalProgram(); boot(); runFor(11);
    startPresses.push_back({simUs, simUs + 25000}); startPresses.push_back({simUs + 50000, simUs + 200000}); startAt = now();
    runFor(2);
    CHECK(activeState != stIdle && !noSaveNow(), "bounce: cycle not started in Reuse and save (state %d, noSave %d)", activeState, noSaveNow());
    CHECK(eventCountOf("Cycle started") == 1 && eventCountOf("Mode changed") == 0, "bounce counted twice: %d starts, %d mode changes",
          eventCountOf("Cycle started"), eventCountOf("Mode changed"));
    // A real press 2 s later does count: it switches the mode.
    tapStart(); runFor(0.1);
    CHECK(noSaveNow(), "a real press 2 s after the start did not switch the mode");
  }
  else if (sc == "F26") {
    printf("F26 START held at power-up, or stuck during a cycle: nothing starts or changes, \"Button stuck\" (3.17)\n");
    startPresses.push_back({0, 25000000});                // held from before power-up until 25 s
    typicalProgram(); boot(); runFor(16);
    CHECK(activeState == stIdle, "START held at power-up started a cycle (state %d)", activeState);
    CHECK(eventSeen("START held 15 s: treated as stuck"), "stuck START at power-up not in the event log");
    CHECK(onScreen("START BUTTON") && onScreen("Button stuck"), "stuck START at power-up not shown on the screen");
    runFor(10);                                           // released at 25 s
    CHECK(activeState == stIdle && eventCountOf("Cycle started") == 0, "releasing a stuck START started a cycle");
    pressStart(); runFor(0.5);
    CHECK(activeState != stIdle, "after the stuck button was released a normal press did not start");
    // A press that then sticks: the cycle started on the press; the stuck button changes nothing more.
    pressStop(); runFor(6);
    holdStartFor(20.0); startAt = now(); runFor(17);
    CHECK(activeState != stIdle && !noSaveNow() && eventCountOf("Mode changed") == 0, "a stuck START changed the cycle (state %d, noSave %d)", activeState, noSaveNow());
    CHECK(eventCountOf("treated as stuck") == 2 && onScreen("Button stuck"), "a START stuck during a cycle is not reported");
    runFor(5);
    CHECK(!noSaveNow() && eventCountOf("Mode changed") == 0, "releasing a stuck START changed the mode");
  }
  else if (sc == "F27a") {
    printf("F27 panel switch to Reuse only, then a power cut (part 1: run until 320 s, then cut power) (3.17)\n");
    typicalProgram(); boot(); runFor(11); pressStart(); runFor(60);
    tapStart(); runFor(260);
    CHECK(activeState == stFill1 && noSaveNow(), "expected Reuse only in step 1 (state %d, noSave %d)", activeState, noSaveNow());
    CycleSnapshot snap; Preferences p; p.getBytes("cyc", &snap, sizeof(snap));
    CHECK(p.getBytesLength("cyc") == sizeof(snap) && snap.state == stFill1 && snap.filled == 1 && snap.mode == cmReuseOnly,
          "snapshot: state %d filled %d mode %d", snap.state, snap.filled, snap.mode);
    Preferences::saveFile(flash);
  }
  else if (sc == "F27b") {
    printf("F27 panel switch to Reuse only, then a power cut (part 2: power back, resume: still Reuse only)\n");
    Preferences::loadFile(flash);
    simUs += 400000000ULL;
    typicalProgram(); simReset = ESP_RST_POWERON; boot();
    startAt = now() - 320;
    runFor(10);
    CHECK(interruptedNoSave(), "the cut cycle is not reported as Reuse only");
    CHECK(requestResume(), "Resume refused");
    runFor(1);
    CHECK(activeState == stFill1 && noSaveNow(), "resumed cycle is not Reuse only in step 1 (state %d)", activeState);
    CHECK(eventSeen("Cycle resumed: step 1/4, reuse only"), "resume not in the event log as Reuse only");
    runFor(1500);
    CHECK(activeState == stIdle && stateAt(stDrain2) > 0, "resumed cycle did not run through step 4");
    CHECK(!rec.savedWhileSaving && stats.totalSavedL == 0, "rinse water saved after resuming a Reuse only cycle (%.1f L)", stats.totalSavedL);
  }
  else if (sc == "F28") {
    printf("F28 the app starts Reuse only, panel presses in step 2: Reuse all, then Reuse and save (3.17, 3.18)\n");
    typicalProgram(); boot(); runFor(11);
    cfg.allowRemoteStart = true;
    CHECK(requestRemoteStart(cmReuseOnly), "Reuse only refused at Ready");
    startAt = now(); runFor(1);
    CHECK(noSaveNow() && eventSeen("Cycle started: reuse only"), "app start was not Reuse only");
    runUntilState(stDrain1); runFor(20);
    CHECK(activeState == stDrain1, "expected step 2, state %d", activeState);
    tapStart(); runFor(0.1);
    CHECK(modeNow() == cmReuseAll && onScreen("REUSE ALL") && eventSeen("Mode changed: reuse all (panel)"), "panel press in step 2 did not switch to Reuse all (mode %d)", modeNow());
    runFor(1.5); tapStart(); runFor(0.1);
    CHECK(!noSaveNow() && onScreen("REUSE & SAVE") && eventSeen("Mode changed: reuse and save (panel)"), "2nd panel press in step 2 did not switch to Reuse and save");
    runFor(1800);
    CHECK(rec.savedWhileSaving && stats.totalSavedL > 1, "step 4 did not save (saved %.1f L)", stats.totalSavedL);
  }
  else if (sc == "F29") {
    printf("F29 STOP after a panel switch stops the cycle, pump/fill/wash off (3.17)\n");
    typicalProgram(); boot(); runFor(11); pressStart(); runFor(300);
    tapStart(); runFor(1.5);
    CHECK(noSaveNow(), "the press did not switch to Reuse only");
    pressStop(); runFor(3);
    CHECK(activeState == stIdle, "not back to Ready after STOP");
    CHECK(pinOut[OUT_MOTOR] == LOW && pinOut[OUT_FILL_EV] == LOW && pinOut[OUT_WASH_EV] == LOW, "pump, fill or wash output still on");
    CHECK(eventSeen("Cycle stopped") && !noSaveNow(), "stop not logged, or Reuse only still reported");
    Preferences p; CHECK(p.getBytesLength("cyc") == 0, "cycle snapshot left behind after STOP");
  }
  else if (sc == "F16a") {
    printf("F16 Reuse only cycle cut after the machine filled in step 1 (part 1: run until 320 s, then cut power) (3.10)\n");
    typicalProgram(); boot();
    cfg.allowRemoteStart = true;
    CHECK(requestRemoteStart(cmReuseOnly), "Reuse only refused at Ready");
    startAt = now(); runFor(320);
    CHECK(activeState == stFill1, "expected to be washing, state %d", activeState);
    CycleSnapshot snap; Preferences p;
    CHECK(p.getBytesLength("cyc") == sizeof(snap), "no full snapshot in flash during the cycle");
    p.getBytes("cyc", &snap, sizeof(snap));
    CHECK(snap.version == CYCLE_SNAP_VERSION && snap.state == stFill1 && snap.filled == 1 && snap.mode == cmReuseOnly,
          "snapshot: version %d state %d filled %d mode %d", snap.version, snap.state, snap.filled, snap.mode);
    CHECK(snap.reusedL > 5 && snap.cycleS >= 130, "snapshot water/time: reused %.1f L, %u s", snap.reusedL, (unsigned)snap.cycleS);
    Preferences::saveFile(flash);
  }
  else if (sc == "F16b") {
    printf("F16 Reuse only cycle cut after the machine filled in step 1 (part 2: power back, resume from the app)\n");
    Preferences::loadFile(flash);
    CycleSnapshot saved; { Preferences p; p.getBytes("cyc", &saved, sizeof(saved)); }
    simUs += 400000000ULL;                               // the clock of this run starts later than the washer program
    typicalProgram(); simReset = ESP_RST_POWERON; boot();
    startAt = now() - 320;                               // the washer kept its own program; power was off a few seconds
    runFor(10);                                          // start-up animation, then both pages of the INTERRUPTED notice
    int st; uint32_t ep;
    CHECK(interruptedInfo(st, ep) && st == stFill1 && interruptedNoSave(), "interruption or its mode not detected (step %d)", st);
    CHECK(activeState == stIdle && pinOut[OUT_MOTOR] == LOW && pinOut[OUT_FILL_EV] == LOW && pinOut[OUT_WASH_EV] == LOW,
          "not safely in Ready after power-up");
    CHECK(resumeAvailable() && resumeLeftS() > 1770 && resumeLeftS() <= 1800, "Resume not offered (%u s left)", (unsigned)resumeLeftS());
    CHECK(onScreen("Resume in app") || sawMsg("Resume in app"), "screen does not say Resume in app");
    CHECK(eventSeen("Resume offered in the app for 30 min"), "event log does not say the cycle can be resumed");
    CHECK(requestResume(), "Resume refused");
    runFor(1);
    CHECK(activeState == stFill1, "not back in step 1 after Resume (state %d)", activeState);
    CHECK(eventSeen("Cycle resumed: step 1/4, reuse only, was filled"), "resume not in the event log");
    CHECK(onScreen("RESUMED"), "screen does not show RESUMED");
    CHECK(!interruptedInfo(st, ep) && !resumeAvailable(), "interrupted notice still shown after Resume");
    SystemStatus snap; getSystemStatus(snap);
    CHECK(snap.noSave && snap.cycleReusedL >= saved.reusedL - 0.01f, "mode or water not carried over (noSave %d, reused %.1f of %.1f L)",
          snap.noSave, snap.cycleReusedL, saved.reusedL);
    { CycleSnapshot now_; Preferences p; p.getBytes("cyc", &now_, sizeof(now_));
      CHECK(p.getBytesLength("cyc") == sizeof(now_) && now_.state == stFill1 && now_.filled == 1, "snapshot not written again after Resume"); }
    CHECK(!requestResume(), "a second Resume accepted while the cycle runs");
    runFor(1500);
    double d1 = stateAt(stDrain1), f2 = stateAt(stFill2), d2 = stateAt(stDrain2);
    printf("    after Resume: soapy drain %.1f s, rinse %.1f s, final drain %.1f s\n", d1, f2, d2);
    CHECK(d1 >= 600 && d1 <= 607, "step 1 did not end with the wash drain at 600 s (%.1f s): the fill before the cut was not kept", d1);
    CHECK(f2 >= 780 && f2 <= 787 && d2 >= 1500 && d2 <= 1507, "later steps off: rinse %.1f s, final drain %.1f s", f2, d2);
    CHECK(!rec.savedWhileSaving, "rinse water went to the tank in a resumed Reuse only cycle");
    CHECK(activeState == stIdle && stats.totalCycles == 1, "resumed cycle did not complete");
    CHECK(stats.totalReusedL >= saved.reusedL - 0.01f && stats.totalSavedL == 0, "history: reused %.1f L (before the cut %.1f), saved %.1f L",
          stats.totalReusedL, saved.reusedL, stats.totalSavedL);
    CHECK(stats.stepSamples[0] == 0 && stats.stepSamples[1] == 1, "a resumed step was used for the typical times (%u, %u)",
          (unsigned)stats.stepSamples[0], (unsigned)stats.stepSamples[1]);
    CHECK(alertSeen("Reuse Water: cycle complete"), "no cycle-complete alert");
    Preferences p; CHECK(p.getBytesLength("cyc") == 0, "snapshot left behind after the resumed cycle ended");
  }
  else if (sc == "F18") {
    printf("F18 128x64 two-colour screen: header in the yellow rows, two message lines in the blue rows (3.12)\n");
    boot(); runFor(11);                                    // past the start-up animation (7.4 s) and notice
    CHECK(oled.height == 64, "screen height is %d rows, not 64", oled.height);
    int title = rowOf("READY"), top = rowOf("Start washer"), bottom = rowOf("Press START");
    CHECK(title >= 0 && title + 8 <= 16, "header at row %d: not inside the yellow rows 0-15", title);
    CHECK(top >= 16 && bottom > top && bottom < 64, "the two pages are not on two lines in the blue rows (%d, %d)", top, bottom);
    // Three pages: the first stays on the top line, the others take turns under it.
    displayShowNotice("TEST", "One|Two|Three", 1000);
    runFor(0.2);
    CHECK(rowOf("TEST") == title && rowOf("One") == top && rowOf("Two") == bottom && rowOf("Three") < 0, "3 pages: first screen is not One / Two");
    runFor(DISPLAY_PAGE_MS / 1000.0);
    CHECK(rowOf("One") == top && rowOf("Three") == bottom && rowOf("Two") < 0, "3 pages: second screen is not One / Three (notice too short, or the top line moved)");
    runFor(DISPLAY_PAGE_MS / 1000.0 + 0.2);
    CHECK(rowOf("Start washer") == top && rowOf("One") < 0, "notice did not end after showing every page");
    // One page: alone, half-way between the two lines.
    displayShowNotice("TEST", "Alone", 2000); runFor(0.2);
    int alone = rowOf("Alone");
    CHECK(alone > top && alone < bottom, "a one-page message is at row %d, not between the two lines (%d, %d)", alone, top, bottom);
    // A countdown on the bottom line does not restart the pages.
    displayShowNotice("TEST", "One|Two|9 min left", 20000); runFor(DISPLAY_PAGE_MS / 1000.0 + 0.2);
    CHECK(rowOf("9 min left") == bottom, "third page not shown after one page time");
    displayShowNotice("TEST", "One|Two|8 min left", 20000); runFor(0.2);
    CHECK(rowOf("8 min left") == bottom && rowOf("Two") < 0, "a new number in the countdown restarted the pages");
  }
  else if (sc == "F20") {
    printf("F20 start-up: the collie and the butterfly (about 4.5 s, 25 fps), then System ready with the version, every message on two lines at most (3.12)\n");
    typicalProgram(); int sent0 = oled.displays; boot();   // boot() runs 1 s: the scene is playing
    CHECK(oled.shown.empty(), "text on the screen during the start-up scene");
    runFor(2.5); CHECK(oled.shown.empty(), "the scene ended before 3.5 s");
    int frames = oled.displays - sent0;
    CHECK(frames >= 3.5 * 25 * 0.9 && frames <= 3.5 * 25 * 1.1, "%d frames sent in 3.5 s: not 25 per second", frames);
    runFor(2.0);
    CHECK(onScreen("REUSE WATER") && onScreen("System ready") && onScreen("Version " FIRMWARE_VERSION),
          "after the scene the screen does not show REUSE WATER / System ready / Version " FIRMWARE_VERSION);
    pressStart(); runFor(1800);
    for (auto& m : rec.msgs) CHECK(std::count(m.begin(), m.end(), '|') <= 1, "message with more than two lines: \"%s\"", m.c_str());
    CHECK(stats.totalCycles == 1, "cycle did not complete");
  }
  else if (sc == "F21a") {
    printf("F21 estimated tank litres (part 1: a cycle, the gauge, FULL and EMPTY; 3.13)\n");
    tankL = 30.0; typicalProgram(); boot(); runFor(11);
    CHECK(fabsf(tankLitres() - 25.0f) < 0.6f, "no saved estimate: start guess %.1f L, expected 25 (between the floats)", tankLitres());
    CHECK(onScreen("25L"), "the header does not show 25L");
    pressStart();
    float minErr = 0, maxErr = 0; bool sawDown = false, sawUp = false, sawUpArrowState = false;
    double prevTank = tankL;
    for (int i = 0; i < 1700 * 4; i++) {
      runFor(0.25);
      float err = tankLitres() - (float)tankL;
      if (err < minErr) minErr = err;
      if (err > maxErr) maxErr = err;
      if (tankTrend() < 0 && tankL < prevTank) sawDown = true;
      if (tankTrend() > 0 && tankL > prevTank) { sawUp = true; if (activeState == stDrain2) sawUpArrowState = true; }
      prevTank = tankL;
      if (activeState == stIdle && i > 40) break;
    }
    printf("    estimate minus real tank: %.1f to %.1f L; real tank at the end %.1f L, estimate %.1f L\n", minErr, maxErr, tankL, tankLitres());
    CHECK(maxErr - minErr < 12.0f, "the estimate drifts %.1f L from the real tank", maxErr - minErr);
    CHECK(fabsf(tankLitres() - (float)tankL) < 3.0f, "at the end of the cycle the estimate is %.1f L, the tank %.1f L", tankLitres(), tankL);
    CHECK(sawDown, "the trend never showed draining while the pump drew from the tank");
    CHECK(sawUp && sawUpArrowState, "the trend never showed filling while rinse water was saved");
    { Preferences q; float e = tankLitres(); q.putBytes("simExpect", &e, sizeof(e)); }
    Preferences::saveFile(flash);
    // FULL and EMPTY come from the floats.
    tankL = 43.0; runFor(3);
    CHECK(tankLitres() >= TANK_HIGH_MARK_L - 0.01f && onScreen("FULL"), "high float wet: estimate %.1f L, header does not say FULL", tankLitres());
    tankL = 5.0; runFor(6);
    CHECK(tankLitres() <= TANK_LOW_MARK_L + 0.01f && onScreen("EMPTY"), "low float dry: estimate %.1f L, header does not say EMPTY", tankLitres());
  }
  else if (sc == "F21b") {
    printf("F21 estimated tank litres (part 2: power cut, the estimate is kept)\n");
    Preferences::loadFile(flash);
    float expect = -1; { Preferences q; q.getBytes("simExpect", &expect, sizeof(expect)); }
    tankL = expect; boot(); runFor(1);
    CHECK(expect >= 0 && fabsf(tankLitres() - expect) < 0.6f, "after a power cut the estimate is %.1f L, saved %.1f L", tankLitres(), expect);
  }
  else if (sc == "F19") {
    printf("F19 screen at the other I2C address, 0x3D (3.12)\n");
    oledAddress = 0x3D; typicalProgram(); boot();
    CHECK(displayAvailable() && oled.beginAddress == 0x3D, "screen at 0x3D not found (started at 0x%02X)", oled.beginAddress);
    CHECK(!alertSeen("Reuse Water: screen not working") && !eventSeen("Screen not found"), "false missing-screen alert");
    runFor(11); CHECK(onScreen("READY"), "nothing drawn on the screen at 0x3D");
  }
  else if (sc == "F17") {
    printf("F17 Resume limits: 3.9 marker, tank wash, 30 min, dismiss, START, old snapshot (3.10)\n");
    typicalProgram(); boot(); runFor(11);
    Preferences p; int st; uint32_t ep;
    auto cut = [&](uint8_t state, uint32_t epoch) {     // as if the power had just come back with this snapshot
      CycleSnapshot s; memset(&s, 0, sizeof(s)); s.version = CYCLE_SNAP_VERSION; s.state = state; s.epoch = epoch; s.cycleS = 900; s.reusedL = 12;
      p.putBytes("cyc", &s, sizeof(s)); bootChecks();
    };
    struct { uint8_t state; uint32_t epoch; } old = { stFill2, 0 };   // firmware 3.9 marker
    p.putBytes("cyc", &old, sizeof(old)); bootChecks();
    CHECK(interruptedInfo(st, ep) && st == stFill2, "3.9 marker not reported as interrupted");
    CHECK(!resumeAvailable() && !requestResume(), "a 3.9 marker (no mode, no water) offered Resume");
    CHECK(p.getBytesLength("cyc") == 0, "marker not cleared");
    cut(stDrain1, 0);
    CHECK(resumeAvailable(), "Resume not offered for a full snapshot");
    CHECK(startManualWash() == washStarted && !resumeAvailable(), "Resume offered during a tank wash");
    stopManualWash(); runFor(5);
    CHECK(resumeAvailable(), "Resume not offered again after the tank wash");
    runFor(1800);
    CHECK(!resumeAvailable() && resumeLeftS() == 0 && !requestResume(), "Resume still offered 30 min after power-up");
    CHECK(interruptedInfo(st, ep), "interrupted notice gone with the Resume offer");
    cut(stDrain1, 0); dismissInterrupted();
    CHECK(!resumeAvailable(), "Resume offered after the notice was dismissed");
    cut(stFill1, (uint32_t)time(nullptr) - 5 * 3600);
    CHECK(!resumeAvailable(), "Resume offered for a step saved 5 h ago");
    cut(stFill1, (uint32_t)time(nullptr) - 3600);
    CHECK(resumeAvailable(), "Resume not offered for a step saved 1 h ago");
    pressStart(); runFor(2);
    CHECK(activeState != stIdle && !resumeAvailable() && !interruptedInfo(st, ep), "START on the panel did not replace the Resume offer");
    SystemStatus snap; getSystemStatus(snap);
    CHECK(snap.cycleReusedL == 0, "a new cycle started with the cut cycle's water");
  }
  else if (sc == "F12") {
    printf("F12 full cycle in Spanish (factory default language)\n");
    simLanguage = LANG_ES; typicalProgram(); boot();
    CHECK(cfg.language == LANG_ES, "language is not Spanish");
    CHECK(!strcmp(displayGetTitle(), "LISTO") && sawMsg("y pulse START"), "Ready screen: %s / %s", displayGetTitle(), displayGetMessage());
    pressStart(); runFor(1600);
    while (activeState != stIdle && now() < 4000) runFor(0.1);   // stop right at the end of the cycle
    runFor(0.5);
    bool doneShown = false; for (auto& t : oled.shown) if (t == "TERMINADO") doneShown = true;
    CHECK(doneShown, "end-of-cycle screen is not TERMINADO");
    for (const char* m : {"Esperando agua", "Reusando agua", "Lavando...", "Agua jabonosa", "Enjuagando...", "Guardando agua"})
      CHECK(sawMsg(m), "screen never showed \"%s\"", m);
    CHECK(activeState == stIdle && stats.totalCycles == 1, "cycle did not complete");
    int prio = 0;
    CHECK(alertSeen("Reúso de Agua: ciclo completo", &prio) && prio == 2, "Spanish cycle-complete alert missing");
    CHECK(eventSeen("Cycle complete"), "event log should stay in English");
    bool reused = false, saved = false;   // the end notice shows both lines at once (3.12)
    for (auto& t : oled.shown) { if (t.rfind("Reusados ", 0) == 0) reused = true; if (t.rfind("Guardados ", 0) == 0) saved = true; }
    CHECK(reused && saved, "end-of-cycle screen does not show \"Reusados ... L\" and \"Guardados ... L\" together");
  }
  else if (sc == "F6") {
    printf("F6 watchdog restart while Ready (no cycle)\n");
    simReset = ESP_RST_TASK_WDT; boot(); int st; uint32_t ep;
    CHECK(!interruptedInfo(st, ep), "interruption reported although no cycle ran");
    CHECK(eventSeen("Watchdog"), "watchdog restart not in the event log");
  }
  else if (sc == "F30") {
    printf("F30 Reuse all, full cycle from 30 L: tank water in step 1 and step 3, nothing saved (3.18)\n");
    typicalProgram(); boot(); runFor(11);
    cfg.allowRemoteStart = true;
    // First a Reuse only cycle from the same 30 L, to compare the litres reused.
    CHECK(requestRemoteStart(cmReuseOnly), "Reuse only refused at Ready");
    startAt = now(); runFor(1800);
    CHECK(activeState == stIdle && stats.lastMode == cmReuseOnly, "Reuse only cycle did not complete (last mode %d)", stats.lastMode);
    float reuseOnlyL = stats.lastReusedL;
    CHECK(!rec.pumpInState[stFill2], "Reuse only: pump ran in step 3");
    tankL = 30.0; rec = Rec(); alerts.clear();
    CHECK(requestRemoteStart(cmReuseAll), "Reuse all refused at Ready");
    startAt = now(); runFor(1);
    CHECK(modeNow() == cmReuseAll && noSaveNow(), "status: mode %d, noSave %d", modeNow(), noSaveNow());
    CHECK(eventSeen("Cycle started: reuse all, rinse water to the drain"), "event log has no Reuse all start");
    CHECK(sawMsg("Reuse all"), "start screen does not say Reuse all");
    double tank3 = -1; bool reusingOnRinse = false;
    while (activeState != stIdle && now() < startAt + 2000) {
      runFor(0.25);
      if (activeState == stFill2 && tank3 < 0) tank3 = tankL;
      if (activeState == stFill2 && !strcmp(displayGetTitle(), "RINSE 3 OF 4") && onScreen("Reusing water")) reusingOnRinse = true;
    }
    printf("    tank at step 3: %.1f L, at the end %.1f L; reused %.1f L (Reuse only %.1f L)\n", tank3, tankL, stats.lastReusedL, reuseOnlyL);
    CHECK(rec.pumpInState[stFill1] && rec.pumpInState[stFill2], "pump not on in step 1 (%d) and step 3 (%d)", rec.pumpInState[stFill1], rec.pumpInState[stFill2]);
    CHECK(reusingOnRinse, "RINSE 3 OF 4 never showed \"Reusing water\"");
    CHECK(eventSeen("Step 3/4 rinse started (reuse all)"), "event log has no Reuse all step 3");
    CHECK(tankL > 9.0 && tankL < 11.5, "tank ended at %.1f L, not at the low float (10 L)", tankL);
    CHECK(!rec.fillEverOn && !rec.savedWhileSaving && stats.lastSavedL == 0, "Reuse all opened the fill valve or saved %.1f L", stats.lastSavedL);
    CHECK(sawMsg("Not saving"), "step 4 screen never showed \"Not saving\"");
    CHECK(!rec.pumpWhileTap, "pump and tap valve were on together");
    CHECK(stats.lastReusedL > reuseOnlyL + 1, "Reuse all reused %.1f L, not more than Reuse only (%.1f L)", stats.lastReusedL, reuseOnlyL);
    CHECK(stats.lastMode == cmReuseAll && stats.totalReusedL > reuseOnlyL + stats.lastReusedL - 0.1f, "history: last mode %d, total reused %.1f L", stats.lastMode, stats.totalReusedL);
    CHECK(!alertSeen("Reuse Water: tank full"), "tank-full alert in a Reuse all cycle");
    CHECK(modeNow() == cmReuseSave && !noSaveNow(), "Reuse all still reported after the cycle");
  }
  else if (sc == "F31") {
    printf("F31 Reuse all: the tank reaches the low float mid-rinse, step 3 changes to tap water (3.18)\n");
    typicalProgram(); boot(); runFor(11);
    cfg.allowRemoteStart = true;
    CHECK(requestRemoteStart(cmReuseAll), "Reuse all refused at Ready");
    startAt = now(); runUntilState(stFill2);
    tankL = 13.0; rec.msgs.clear(); rec.worstDeadHeadMs = 0; rec.pumpWhileTap = false;   // about 3 L left above the low float
    bool pumped = false, tapAfter = false;
    for (int i = 0; i < 100 * 20; i++) {
      runFor(0.05);
      if (pinOut[OUT_MOTOR] == HIGH) pumped = true;
      if (pumped && pinOut[OUT_MOTOR] == LOW && pinOut[OUT_TAP_EV] == HIGH && progT() < 880) tapAfter = true;
      if (activeState != stFill2) break;
    }
    printf("    tank %.1f L when the rinse filled; worst run against a closed inlet %u ms\n", tankL, rec.worstDeadHeadMs);
    CHECK(pumped, "pump never ran in step 3");
    CHECK(tapAfter, "step 3 did not change to tap water during the draw");
    CHECK(sawMsg("Tank is low"), "\"Tank is low\" not shown in step 3");
    CHECK(!rec.pumpWhileTap, "pump and tap valve were on together");
    CHECK(rec.worstDeadHeadMs <= 350, "pump ran %u ms against the closed inlet", rec.worstDeadHeadMs);
    CHECK(tankL > 9.0, "tank pumped down to %.1f L, below the low float", tankL);
  }
  else if (sc == "F32") {
    printf("F32 Reuse all with the tank below the low float at Start: the whole cycle on tap water (3.18)\n");
    tankL = 5.0; typicalProgram(); boot(); runFor(11);
    cfg.allowRemoteStart = true;
    CHECK(requestRemoteStart(cmReuseAll), "Reuse all refused at Ready");
    startAt = now(); runFor(1800);
    CHECK(activeState == stIdle && stateAt(stDrain2) > 0, "cycle did not complete");
    CHECK(rec.pumpOnFirst < 0, "pump ran with an empty tank");
    CHECK(!rec.fillEverOn && stats.lastSavedL == 0 && stats.lastReusedL == 0 && stats.lastTapL > 20, "litres: tap %.1f, reused %.1f, saved %.1f",
          stats.lastTapL, stats.lastReusedL, stats.lastSavedL);
    CHECK(sawMsg("Tank is low"), "\"Tank is low\" not shown");
  }
  else if (sc == "F33") {
    printf("F33 Reuse and save and Reuse only: step 3 still on tap water only (3.18 regression)\n");
    typicalProgram(); boot(); runFor(11);
    cfg.allowRemoteStart = true;
    for (CycleMode m : {cmReuseSave, cmReuseOnly}) {
      tankL = 30.0; rec = Rec();
      CHECK(requestRemoteStart(m), "start refused (mode %d)", m);
      startAt = now(); runFor(1800);
      CHECK(activeState == stIdle && stateAt(stDrain2) > 0, "mode %d: cycle did not complete", m);
      CHECK(rec.pumpInState[stFill1] && !rec.pumpInState[stFill2], "mode %d: pump in step 1 %d, in step 3 %d", m, rec.pumpInState[stFill1], rec.pumpInState[stFill2]);
      CHECK(m == cmReuseSave ? rec.savedWhileSaving : !rec.fillEverOn, "mode %d: step 4 did not behave as before", m);
    }
  }
  else if (sc == "F34") {
    printf("F34 START presses: 1 Reuse and save, 2 Reuse only, 3 Reuse all, 4 Reuse and save; locked in step 4 (3.18)\n");
    typicalProgram(); boot(); runFor(11); pressStart(); runFor(0.1);
    CHECK(activeState != stIdle && modeNow() == cmReuseSave, "1 press: state %d, mode %d", activeState, modeNow());
    runFor(30);
    tapStart(); runFor(0.1);
    CHECK(modeNow() == cmReuseOnly && onScreen("REUSE ONLY") && eventSeen("Mode changed: reuse only (panel)"), "2 presses: mode %d", modeNow());
    runFor(1.5); tapStart(); runFor(0.1);
    CHECK(modeNow() == cmReuseAll && noSaveNow() && onScreen("REUSE ALL") && onScreen("to change") && eventSeen("Mode changed: reuse all (panel)"),
          "3 presses: mode %d, notice REUSE ALL / to change missing or not logged", modeNow());
    { CycleSnapshot c; Preferences p; p.getBytes("cyc", &c, sizeof(c)); CHECK(c.mode == cmReuseAll, "snapshot mode %d after the switch to Reuse all", c.mode); }
    runFor(1.5); tapStart(); runFor(0.1);
    CHECK(modeNow() == cmReuseSave && !noSaveNow() && eventSeen("Mode changed: reuse and save (panel)"), "4 presses: mode %d", modeNow());
    runUntilState(stDrain2); runFor(5);
    tapStart(); runFor(0.1);
    CHECK(activeState == stDrain2 && modeNow() == cmReuseSave && onScreen("Mode fixed"), "a press in step 4 changed the mode (%d)", modeNow());
    CHECK(eventCountOf("Mode changed") == 3, "%d mode changes logged, expected 3", eventCountOf("Mode changed"));
    runFor(300);
    CHECK(rec.savedWhileSaving, "step 4 did not save after the 4th press");
  }
  else if (sc == "F35") {
    printf("F35 switching to Reuse all during a step 3 draw starts the pump; switching away goes back to tap (3.18)\n");
    typicalProgram(); boot(); runFor(11); pressStart(); runUntilState(stFill2); runFor(5);
    CHECK(activeState == stFill2 && pinOut[OUT_MOTOR] == LOW && pinOut[OUT_TAP_EV] == HIGH, "step 3 in Reuse and save is not on tap water");
    tapStart(); runFor(1.2); tapStart(); runFor(0.1);     // Reuse only, then Reuse all
    CHECK(modeNow() == cmReuseAll, "not Reuse all after two presses (mode %d)", modeNow());
    runFor(2.0);
    CHECK(pinOut[OUT_MOTOR] == HIGH && pinOut[OUT_TAP_EV] == LOW, "pump not on the current draw after switching to Reuse all");
    double tankBefore = tankL; runFor(5);
    CHECK(tankL < tankBefore - 0.3, "tank water not used after the switch (%.1f -> %.1f L)", tankBefore, tankL);
    runFor(1.0); tapStart(); runFor(0.5);                 // back to Reuse and save
    CHECK(modeNow() == cmReuseSave && pinOut[OUT_MOTOR] == LOW && pinOut[OUT_TAP_EV] == HIGH, "switching away from Reuse all did not go back to tap (mode %d, pump %d)",
          modeNow(), pinOut[OUT_MOTOR]);
    CHECK(!rec.pumpWhileTap, "pump and tap valve were on together");
    runFor(1800);
    CHECK(activeState == stIdle && rec.savedWhileSaving, "cycle did not complete and save after switching back");
  }
  else if (sc == "F36a") {
    printf("F36 power cut in step 3 while Reuse all pumps (part 1: cut at 800 s) (3.18)\n");
    typicalProgram(); boot(); runFor(11);
    cfg.allowRemoteStart = true;
    CHECK(requestRemoteStart(cmReuseAll), "Reuse all refused at Ready");
    startAt = now(); runFor(800);
    CHECK(activeState == stFill2 && pinOut[OUT_MOTOR] == HIGH, "expected step 3 with the pump on (state %d, pump %d)", activeState, pinOut[OUT_MOTOR]);
    CycleSnapshot snap; Preferences p; p.getBytes("cyc", &snap, sizeof(snap));
    CHECK(snap.version == CYCLE_SNAP_VERSION && snap.state == stFill2 && snap.mode == cmReuseAll, "snapshot: version %d state %d mode %d", snap.version, snap.state, snap.mode);
    { Preferences q; double t = tankL; q.putBytes("simTank", &t, sizeof(t)); }
    Preferences::saveFile(flash);
  }
  else if (sc == "F36b") {
    printf("F36 power cut in step 3 while Reuse all pumps (part 2: power back, resume: still Reuse all)\n");
    Preferences::loadFile(flash);
    { Preferences q; q.getBytes("simTank", &tankL, sizeof(tankL)); }
    simUs += 900000000ULL;
    typicalProgram(); simReset = ESP_RST_POWERON; boot();
    startAt = now() - 800;
    runFor(5);
    CHECK(interruptedMode() == cmReuseAll && interruptedNoSave(), "the cut cycle is not reported as Reuse all (mode %d)", interruptedMode());
    CHECK(pinOut[OUT_MOTOR] == LOW, "pump on after power-up");
    CHECK(requestResume(), "Resume refused");
    runFor(3);
    CHECK(activeState == stFill2 && modeNow() == cmReuseAll, "resumed cycle: state %d, mode %d", activeState, modeNow());
    CHECK(eventSeen("Cycle resumed: step 3/4, reuse all"), "resume not in the event log as Reuse all");
    CHECK(pinOut[OUT_MOTOR] == HIGH, "the resumed step 3 does not reuse tank water on the current draw");
    runFor(1500);
    CHECK(activeState == stIdle && stateAt(stDrain2) > 0 && !rec.fillEverOn && stats.lastSavedL == 0, "resumed Reuse all cycle did not finish without saving");
    CHECK(stats.lastMode == cmReuseAll, "history: last mode %d", stats.lastMode);
  }
  else if (sc == "F37") {
    printf("F37 a snapshot from 3.10-3.17 (version 1, noSave byte) is read at power-up (3.18)\n");
    typicalProgram(); boot(); runFor(11);
    Preferences p;
    auto cutV1 = [&](uint8_t noSave) {
      CycleSnapshot s; memset(&s, 0, sizeof(s)); s.version = 1; s.state = stFill2; s.mode = noSave; s.cycleS = 790; s.reusedL = 12;
      p.putBytes("cyc", &s, sizeof(s)); bootChecks();
    };
    cutV1(0);
    CHECK(resumeAvailable() && interruptedMode() == cmReuseSave && !interruptedNoSave(), "version 1, noSave 0: resumable %d, mode %d", resumeAvailable(), interruptedMode());
    cutV1(1);
    CHECK(resumeAvailable() && interruptedMode() == cmReuseOnly && interruptedNoSave(), "version 1, noSave 1: resumable %d, mode %d", resumeAvailable(), interruptedMode());
    startAt = now() - 790;                                // the rinse draw (780-880 s) is under way
    CHECK(requestResume(), "Resume of a version 1 snapshot refused");
    runFor(30);
    CHECK(activeState == stFill2 && modeNow() == cmReuseOnly, "resumed: state %d, mode %d", activeState, modeNow());
    CHECK(eventSeen("Cycle resumed: step 3/4, reuse only"), "resume of the old snapshot not logged as Reuse only");
    CHECK(!rec.pumpInState[stFill2], "the resumed Reuse only step 3 used tank water");
  }
  else if (sc == "F38") {
    printf("F38 STOP in step 3 while Reuse all pumps: every output safe (3.18)\n");
    typicalProgram(); boot(); runFor(11);
    cfg.allowRemoteStart = true;
    CHECK(requestRemoteStart(cmReuseAll), "Reuse all refused at Ready");
    startAt = now(); runUntilState(stFill2); runFor(15);
    CHECK(pinOut[OUT_MOTOR] == HIGH, "pump not on in step 3");
    pressStop(); runFor(3);
    CHECK(activeState == stIdle, "not back to Ready after STOP");
    CHECK(pinOut[OUT_MOTOR] == LOW && pinOut[OUT_FILL_EV] == LOW && pinOut[OUT_WASH_EV] == LOW, "pump, fill or wash output still on");
    CHECK(pinOut[OUT_TAP_EV] == HIGH && pinOut[OUT_DRAIN_EV] == HIGH, "tap and drain not back to normal");
    CHECK(modeNow() == cmReuseSave && eventSeen("Cycle stopped"), "stop not logged, or Reuse all still reported");
    Preferences p; CHECK(p.getBytesLength("cyc") == 0, "cycle snapshot left behind after STOP");
  }
  printf("    %d checks passed, %d failed\n", passes, failures);
  return failures ? 1 : 0;
}
