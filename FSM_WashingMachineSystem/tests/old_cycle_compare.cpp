// Firmware 2.3 gating (detector code verbatim) on the same scenarios as test_flow.cpp.
// -1 = the step never finished (stuck).  Build: g++ -std=c++17 old_cycle_compare.cpp -o old_cycle && ./old_cycle
#include <cstdio>
#include <cstdint>
#include <functional>
static unsigned long nowMs=0; static unsigned long millis(){return nowMs;}
static const unsigned long EDGE_DETECTION_DELAY=4000;
bool detectRisingEdge(float value, unsigned long& startTime, bool& detected){
  if (value>0.0f){ if(startTime==0) startTime=millis();
    if(!detected && (millis()-startTime>=EDGE_DETECTION_DELAY)){ detected=true; startTime=0; return true; } }
  else { startTime=0; detected=false; } return false; }
struct R{int f1=-1,d1=-1,f2=-1;};
static R run(std::function<float(uint32_t)> tapL, std::function<float(uint32_t)> drainL, float noiseHz){
  const double TAPPPL=660, DPPL=352; double ta=0,da=0; uint32_t tp=0,dp=0,ltp=0,ldp=0,noise=0;
  unsigned long tt=0,dt=0; bool tdet=false,ddet=false; bool tapOnce=false,drainOnce=false; int step=1; R r;
  for(uint32_t t=10;t<2000000 && step<=3;t+=10){ nowMs=t;
    ta+=tapL(t)/60.0*TAPPPL*0.01; while(ta>=1){ta-=1;++tp;}
    da+=drainL(t)/60.0*DPPL*0.01; while(da>=1){da-=1;++dp;}
    noise+=(uint32_t)(noiseHz*10); while(noise>=1000){noise-=1000;++dp;}
    if(t%250) continue;
    float tv=(tp-ltp)/TAPPPL, dv=(dp-ldp)/DPPL; ltp=tp; ldp=dp;   // litres in the last 250 ms (>0 = flow)
    if(step==1){ if(!tapOnce && detectRisingEdge(tv,tt,tdet)){tapOnce=true;drainOnce=false;}
                 if(!drainOnce && detectRisingEdge(dv,dt,ddet)) drainOnce=true;
                 if(tapOnce&&drainOnce){r.f1=t/1000;step=2;tapOnce=drainOnce=false;} }
    else if(step==2){ if(!tapOnce && detectRisingEdge(tv,tt,tdet)){tapOnce=true;drainOnce=false;}
                 if(tapOnce){r.d1=t/1000;step=3;tapOnce=drainOnce=false;} }
    else { if(!drainOnce && detectRisingEdge(dv,dt,ddet)) drainOnce=true; if(drainOnce){r.f2=t/1000;step=4;} } }
  return r; }
int main(){
  auto tapT=[](uint32_t t){return (t<120000||(t>=780000&&t<880000))?8.0f:0.0f;};
  auto drT=[](uint32_t t){return ((t>=600000&&t<720000)||(t>=1500000&&t<1620000))?18.0f:0.0f;};
  auto p=[](const char*n,R r){printf("%s fill1=%d drain1=%d fill2=%d\n",n,r.f1,r.d1,r.f2);};
  p("S1 typical      ",run(tapT,drT,0));
  p("S2 trickle      ",run(tapT,[&](uint32_t t){float d=drT(t);return d>0?d:(t>720000?0.3f:0.0f);},0));
  p("S3 noise 2/s    ",run(tapT,drT,2.0f));
  p("S4 slow gaps    ",run(tapT,[](uint32_t t){bool on=(t>=600000&&t<720000)||(t>=1500000&&t<1620000);return (on&&(t%1000)<700)?3.0f:0.0f;},0));
  p("S6 overlap      ",run([](uint32_t t){return (t<120000||(t>=700000&&t<800000))?8.0f:0.0f;},drT,0));
  p("S2b pulses 5/s  ",run(tapT,[&](uint32_t t){float d=drT(t);return d>0?d:(t>720000?0.85f:0.0f);},0));
  p("S7 trickle 1.5  ",run(tapT,[&](uint32_t t){float d=drT(t);return d>0?d:(t>720000?1.5f:0.0f);},0));
  p("S8 60 Hz pickup ",run(tapT,drT,60.0f));
}
