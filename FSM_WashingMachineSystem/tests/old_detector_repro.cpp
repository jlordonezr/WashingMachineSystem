// Reproduces the drain-detection bug using the detector code exactly as it is in
// GreyWaterSystem.cpp (firmware 2.3), driven by simulated 250 ms flow readings.
#include <cstdio>
#include <cstdint>
static unsigned long nowMs = 0;
static unsigned long millis() { return nowMs; }
static const unsigned long EDGE_DETECTION_DELAY = 4000;
// ---- copied verbatim from GreyWaterSystem.cpp ----
bool detectRisingEdge(float value, unsigned long& startTime, bool& detected) {
  if (value > 0.0f) {
    if (startTime == 0) startTime = millis();
    if (!detected && (millis() - startTime >= EDGE_DETECTION_DELAY)) {
      detected = true; startTime = 0; return true;
    }
  } else { startTime = 0; detected = false; }
  return false;
}
// ----
struct Det { unsigned long t=0; bool det=false; };
// Run one "state" for `ms`, calling the detector every 250 ms only while the state
// still waits (like isWashingMachineDrained() with !drainWaterEdgeDetectedOnce).
static bool runState(Det& d, bool callDetector, float (*flow)(unsigned long), unsigned long ms, unsigned long& doneAt) {
  bool seen=false; unsigned long end=nowMs+ms;
  for (; nowMs<end; nowMs+=250) {
    float v=flow(nowMs);
    if (callDetector && !seen && detectRisingEdge(v,d.t,d.det)) { seen=true; doneAt=nowMs; }
  }
  return seen;
}
static unsigned long T0;
static float strongDrain(unsigned long){ return 0.30f; }                      // ~18 L/min
static float noFlow(unsigned long){ return 0.0f; }
static float trickleNoise(unsigned long t){ return ((t/250)%3)? 0.002f:0.002f; } // a pulse in every window
static float trickleThenDrain(unsigned long t){ return (t-T0<120000)? 0.002f : 0.30f; } // trickle 2 min, then rinse drain
static float unplugThenDrain(unsigned long t){ unsigned long r=t-T0; if(r<60000) return 0.002f; if(r<61000) return 0.0f; return 0.30f; }
int main(){
  unsigned long at=0; bool ok;
  // Case A: drain stops completely before the rinse drain  -> expected to work
  { Det d; nowMs=1000; runState(d,true,strongDrain,10000,at);            // Fill1 sees soapy drain, latches
    runState(d,false,strongDrain,60000,at);                                // Drain1: detector not called
    runState(d,false,noFlow,1000,at);
    T0=nowMs; ok=runState(d,true,noFlow,5000,at); ok=runState(d,true,strongDrain,20000,at);
    printf("Case A  drain fully stops before rinse drain : Fill2 %s\n", ok?"ADVANCES (ok)":"STUCK"); }
  // Case B: a trickle / noise pulse keeps the reading above zero into Fill2 -> stuck
  { Det d; nowMs=1000; runState(d,true,strongDrain,10000,at); runState(d,false,strongDrain,60000,at);
    T0=nowMs; ok=runState(d,true,trickleThenDrain,600000,at);
    printf("Case B  trickle/noise between drains          : Fill2 %s\n", ok?"ADVANCES":"STUCK for 10 min (bug reproduced)"); }
  // Case C: the user's workaround: unplug for 1 s, plug back
  { Det d; nowMs=1000; runState(d,true,strongDrain,10000,at); runState(d,false,strongDrain,60000,at);
    T0=nowMs; ok=runState(d,true,unplugThenDrain,120000,at);
    printf("Case C  unplug sensor 1 s, then plug back     : Fill2 %s %.1f s after replug\n", ok?"ADVANCES":"STUCK", ok? (at-T0-61000)/1000.0:0); }
  // Case D: slow pulsed drain (some 250 ms windows have no pulse) -> never qualifies
  { Det d; nowMs=1000; T0=nowMs; ok=runState(d,true,[](unsigned long t){return ((t/250)%4==0)?0.0f:0.02f;},120000,at);
    printf("Case D  slow flow with gaps (1 empty window/s) : %s\n", ok?"detected":"NEVER detected in 2 min (bug)"); }
}
