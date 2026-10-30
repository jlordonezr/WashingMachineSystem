#ifndef GREYWATER_SENSORS_H
#define GREYWATER_SENSORS_H

// Flow sensors: interrupts count pulses; FlowMeter (FlowMeter.h) turns the
// counts into rates, "flowing" states, flow starts and litres.
// The float switches live in GreyWaterOutputs, because the safety task needs them.

#include <Arduino.h>
#include "FlowMeter.h"

extern volatile uint32_t tapPulseCount;     // incremented by the interrupts (glitch-filtered)
extern volatile uint32_t drainPulseCount;

void  flowSensorsBegin();                   // pin modes and interrupts (call in setup)
void  flowSample(float& tapL, float& drainL); // call every loop(): updates the meters, returns litres since the last call
const FlowMeter& tapFlow();
const FlowMeter& drainFlow();
float getTapFlowRate();                     // tap flow in L/s (2 s average)
float getDrainFlowRate();                   // drain flow in L/s (2 s average)

#endif
