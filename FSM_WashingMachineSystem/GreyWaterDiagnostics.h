#ifndef GREYWATER_DIAGNOSTICS_H
#define GREYWATER_DIAGNOSTICS_H

// Event log, loop timing, optional buzzer and restart reason (loop() only).
// The functions are also declared in GreyWaterSystem.h for older code.

#include <Arduino.h>

extern const uint16_t BEEP_DONE[];    // two short beeps: cycle finished
extern const uint16_t BEEP_ALARM[];   // three long beeps: something needs attention
void beep(const uint16_t* pattern);   // non-blocking; silent unless the buzzer is on in Settings
void buzzerUpdate();                  // call every loop() pass

#endif
