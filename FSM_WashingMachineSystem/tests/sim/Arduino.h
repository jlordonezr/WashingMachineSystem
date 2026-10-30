// Minimal Arduino API for running the real firmware on a PC (tests/sim).
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <cstdarg>
#include <cstddef>
typedef uint8_t byte;
class __FlashStringHelper;
#define F(x) (reinterpret_cast<const __FlashStringHelper*>(x))
#define PROGMEM
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define FALLING 2
#define RISING 3
#define CHANGE 4
#define IRAM_ATTR
enum { D0 = 0, D1, D2, D3, D4, D5, D6, D7, D8, D9, D10, D11, D12, D13 };
uint32_t millis();
uint32_t micros();
void delay(uint32_t ms);
int  digitalRead(int pin);
void digitalWrite(int pin, int value);
void pinMode(int pin, int mode);
inline int digitalPinToInterrupt(int pin) { return pin; }
void attachInterrupt(int pin, void (*fn)(), int mode);
inline void enableLoopWDT() {}
inline void disableLoopWDT() {}
inline void feedLoopWDT() {}
struct SerialStub {
  void begin(long) {}
  template <class T> void print(T) {}
  template <class T> void println(T) {}
};
extern SerialStub Serial;

// strlcpy/strlcat: present on the ESP32 and macOS, missing from glibc before 2.38.
#if defined(__GLIBC__) && !((__GLIBC__ > 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 38))
inline size_t strlcpy(char* d, const char* s, size_t n) { size_t l = strlen(s); if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; } return l; }
inline size_t strlcat(char* d, const char* s, size_t n) { size_t l = strnlen(d, n); return l == n ? n + strlen(s) : l + strlcpy(d + l, s, n - l); }
#endif
