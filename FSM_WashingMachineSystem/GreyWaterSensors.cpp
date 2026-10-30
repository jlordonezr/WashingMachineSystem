#include "GreyWaterSensors.h"
#include "GreyWaterSystem.h"
#include "GreyWaterSettings.h"

// The interrupts only count pulses. Everything else (rate, flowing or not,
// flow starts, litres) is worked out in FlowMeter.h, which the PC test suite
// (tests/test_flow.cpp) checks.
volatile uint32_t tapPulseCount   = 0;
volatile uint32_t drainPulseCount = 0;
static uint32_t tapLastPulseUs   = 0;
static uint32_t drainLastPulseUs = 0;
static FlowMeter tapMeter(TAP_PULSES_PER_LITRE);
static FlowMeter drainMeter(DRAIN_PULSES_PER_LITRE);

static void IRAM_ATTR tapPulseISR() {
  if (flowPulseAccept((uint32_t)micros(), tapLastPulseUs, FLOW_GLITCH_US)) tapPulseCount = tapPulseCount + 1;  // only this ISR writes it
}
static void IRAM_ATTR drainPulseISR() {
  if (flowPulseAccept((uint32_t)micros(), drainLastPulseUs, FLOW_GLITCH_US)) drainPulseCount = drainPulseCount + 1;  // only this ISR writes it
}

void flowSensorsBegin() {
  // Internal pull-up on: works with the recommended interface (diode from the
  // sensor output, 10 kOhm pull-up to 3V3). The old FlowSensor library also
  // enabled it, because its begin(fn, pullup) parameter is inverted.
  pinMode(TAP_PIN,   INPUT_PULLUP);
  pinMode(DRAIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TAP_PIN),   tapPulseISR,   RISING);
  attachInterrupt(digitalPinToInterrupt(DRAIN_PIN), drainPulseISR, RISING);
}

void flowSample(float& tapL, float& drainL) {
  float on = cfg.flowOnLpm;
  tapMeter.configure(on, on, cfg.edgeDetectMs, FLOW_GAP_MS, FLOW_STOP_MS);
  drainMeter.configure(on, on, cfg.edgeDetectMs, FLOW_GAP_MS, FLOW_STOP_MS);
  uint32_t now = millis();
  bool tapWas = tapMeter.flowing(), drainWas = drainMeter.flowing();
  tapL   = tapMeter.update(tapPulseCount, now);       // 32-bit reads are atomic on the ESP32
  drainL = drainMeter.update(drainPulseCount, now);
  if (tapMeter.flowing() != tapWas)     { DEBUG_PRINT(tapMeter.flowing()   ? "Tap flow started."   : "Tap flow stopped."); }
  if (drainMeter.flowing() != drainWas) { DEBUG_PRINT(drainMeter.flowing() ? "Drain flow started." : "Drain flow stopped."); }
}

const FlowMeter& tapFlow()   { return tapMeter; }
const FlowMeter& drainFlow() { return drainMeter; }
float getTapFlowRate()   { return tapMeter.rateLpm()   / 60.0f; }
float getDrainFlowRate() { return drainMeter.rateLpm() / 60.0f; }
