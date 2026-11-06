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
bool oledFailBegin = false;
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
static uint64_t startHeldUntil = 0, stopHeldUntil = 0;
static bool lowStuckDry = false;            // F7: the low float has failed and always reads dry
static bool highBobs = false;               // F8: between 38 and 41 L the high float bobs every 300 ms
int digitalRead(int p) {
  if (p == IN_LOWLEVEL)  return (!lowStuckDry && tankL > 10.0) ? HIGH : LOW;
  if (p == IN_HIGHLEVEL) {
    if (highBobs && tankL > 38.0 && tankL < 41.0) return ((simUs / 300000ULL) % 2) ? HIGH : LOW;
    return tankL > 40.0 ? HIGH : LOW;
  }
  if (p == START_BUTTON) return simUs < startHeldUntil ? LOW : HIGH;
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
  if (pumpOn && tapOpen) rec.pumpWhileTap = true;
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
static void pressStart() { startHeldUntil = simUs + 150000; startAt = now(); }
static void pressStop()  { stopHeldUntil  = simUs + 150000; }
// Holds START for the given time; the washer program starts when it is released (3.8).
static void holdStart(double s) { startHeldUntil = simUs + (uint64_t)(s * 1e6); startAt = now() + s; }
static bool onScreen(const char* t) { for (auto& x : oled.shown) if (x == t) return true; return false; }
static double stateAt(int st) { for (auto& s : rec.states) if (s.first == st) return s.second; return -1; }
// True if any screen message had m as one of its pages ("a|b|c").
static bool sawMsg(const char* m) {
  for (auto& msg : rec.msgs) { std::string all = "|" + msg + "|"; if (all.find("|" + std::string(m) + "|") != std::string::npos) return true; }
  return false;
}
static bool eventSeen(const char* needle) { for (int i = 0; i < eventCount(); ++i) { uint32_t e, u; const char* tx; eventAt(i, e, u, tx); if (strstr(tx, needle)) return true; } return false; }
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
    const char* expected[] = {"Waiting to fill", "Reusing water", "Washing...", "Draining", "Rinsing...", "Filling tank"};
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
    Preferences::loadFile(flash); simReset = ESP_RST_POWERON; boot(); runFor(7);   // after the start-up animation and "System ready"
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
    // Every screen font from Settings redraws the Ready screen at once, with the same text (3.4).
    runFor(8);                                             // past the start-up animation and notice
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
    oledFailBegin = true; typicalProgram(); boot();
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
    CHECK(!requestRemoteStart(false), "Reuse only accepted with starting from the app turned off");
    cfg.allowRemoteStart = true;
    CHECK(requestRemoteStart(false), "Reuse only refused at Ready");
    startAt = now();                                   // the washer program starts with the app start
    runFor(5);
    CHECK(systemBusy() && eventSeen("Cycle started: reuse only"), "Reuse only cycle did not start");
    CHECK(!requestRemoteStart(true), "a second start accepted while a cycle runs");
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
    printf("F15 START on the panel: a short press is Reuse and save, held 2 s Reuse only, held 15 s nothing (3.8)\n");
    typicalProgram(); boot();                             // the start-up animation is still running
    // During the start-up animation a press ends it and shows the choice; the release starts the cycle.
    holdStart(0.5); runFor(0.2);
    CHECK(onScreen("REUSE AND SAVE"), "a press during the start-up animation does not show REUSE AND SAVE");
    runFor(1);
    CHECK(activeState != stIdle, "a short press during the start-up animation did not start the cycle");
    pressStop(); runFor(6);
    // Short press: the screen says what a release starts; the cycle starts on release and saves.
    holdStart(1.0); runFor(0.3);
    CHECK(onScreen("REUSE AND SAVE") && onScreen("Release to start"), "short press: screen does not show REUSE AND SAVE / Release to start");
    CHECK(activeState == stIdle, "the cycle started while START was still held");
    runFor(1.0);
    SystemStatus snap; getSystemStatus(snap);
    CHECK(activeState != stIdle && !snap.noSave, "short press did not start Reuse and save (state %d, noSave %d)", activeState, snap.noSave);
    pressStop(); runFor(1);
    CHECK(activeState == stIdle, "STOP did not end the cycle");
    runFor(5);                                             // past the STOPPED notice
    // Held 2 s: the screen switches to REUSE ONLY at 2 s; the release starts Reuse only.
    holdStart(3.0); runFor(1.8);
    CHECK(onScreen("REUSE AND SAVE"), "at 1.8 s the screen should still show REUSE AND SAVE");
    runFor(0.4);
    CHECK(onScreen("REUSE ONLY") && onScreen("Release to start"), "at 2.2 s the screen does not show REUSE ONLY / Release to start");
    CHECK(activeState == stIdle, "the cycle started before START was released");
    runFor(1.5);
    getSystemStatus(snap);
    CHECK(activeState != stIdle && snap.noSave, "START held 3 s did not start Reuse only (state %d, noSave %d)", activeState, snap.noSave);
    CHECK(eventSeen("Cycle started: reuse only"), "event log has no reuse-only start from the panel");
    pressStop(); runFor(6);
    // Held 20 s: stuck; nothing starts, before or after the release.
    holdStart(20.0); runFor(16);
    CHECK(onScreen("START BUTTON") && onScreen("Stuck"), "a stuck START is not reported on the screen");
    runFor(6);
    CHECK(activeState == stIdle, "a 20 s hold started a cycle (state %d)", activeState);
    CHECK(eventSeen("START held 15 s: treated as stuck"), "stuck START not in the event log");
  }
  else if (sc == "F16a") {
    printf("F16 Reuse only cycle cut after the machine filled in step 1 (part 1: run until 320 s, then cut power) (3.10)\n");
    typicalProgram(); boot();
    cfg.allowRemoteStart = true;
    CHECK(requestRemoteStart(false), "Reuse only refused at Ready");
    startAt = now(); runFor(320);
    CHECK(activeState == stFill1, "expected to be washing, state %d", activeState);
    CycleSnapshot snap; Preferences p;
    CHECK(p.getBytesLength("cyc") == sizeof(snap), "no full snapshot in flash during the cycle");
    p.getBytes("cyc", &snap, sizeof(snap));
    CHECK(snap.version == CYCLE_SNAP_VERSION && snap.state == stFill1 && snap.filled == 1 && snap.noSave == 1,
          "snapshot: version %d state %d filled %d noSave %d", snap.version, snap.state, snap.filled, snap.noSave);
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
  else if (sc == "F17") {
    printf("F17 Resume limits: 3.9 marker, tank wash, 30 min, dismiss, START, old snapshot (3.10)\n");
    typicalProgram(); boot(); runFor(7);
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
    for (const char* m : {"Esperando agua", "Reusando agua", "Lavando...", "Desaguando", "Enjuagando...", "Agua al tanque"})
      CHECK(sawMsg(m), "screen never showed \"%s\"", m);
    CHECK(activeState == stIdle && stats.totalCycles == 1, "cycle did not complete");
    int prio = 0;
    CHECK(alertSeen("Reúso de Agua: ciclo completo", &prio) && prio == 2, "Spanish cycle-complete alert missing");
    CHECK(eventSeen("Cycle complete"), "event log should stay in English");
    bool reused = false, saved = false;   // the end notice shows its pages one after the other
    for (int i = 0; i < 100; ++i) {
      runFor(0.1);
      for (auto& t : oled.shown) { if (t.rfind("Reusados ", 0) == 0) reused = true; if (t.rfind("Guardados ", 0) == 0) saved = true; }
    }
    CHECK(reused && saved, "end-of-cycle screen does not page \"Reusados ... L\" and \"Guardados ... L\"");
  }
  else if (sc == "F6") {
    printf("F6 watchdog restart while Ready (no cycle)\n");
    simReset = ESP_RST_TASK_WDT; boot(); int st; uint32_t ep;
    CHECK(!interruptedInfo(st, ep), "interruption reported although no cycle ran");
    CHECK(eventSeen("Watchdog"), "watchdog restart not in the event log");
  }
  printf("    %d checks passed, %d failed\n", passes, failures);
  return failures ? 1 : 0;
}
