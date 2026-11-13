#pragma once
#include <stdint.h>
extern int oledAddress;      // tests: the I2C address the screen answers at (0 = no screen)
struct TwoWire {
  int probing = 0;
  void begin() {}
  void setClock(long) {}
  void beginTransmission(uint8_t address) { probing = address; }
  uint8_t endTransmission() { return probing == oledAddress ? 0 : 2; }   // 2 = address not acknowledged
};
extern TwoWire Wire;
