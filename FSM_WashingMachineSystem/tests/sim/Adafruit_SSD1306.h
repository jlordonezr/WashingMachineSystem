// Records what the firmware prints, so tests can read the screen.
#pragma once
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <string>
#include <vector>
#define WHITE 1
#define BLACK 0
#define SSD1306_SWITCHCAPVCC 2
extern bool oledFailBegin;   // tests: the screen does not answer
struct Adafruit_SSD1306 {
  std::vector<std::string> frame, shown;
  Adafruit_SSD1306(int, int, TwoWire*, int) {}
  bool begin(int, int) { return !oledFailBegin; }
  void setFont(const GFXfont* = nullptr) {}
  void setTextSize(int) {}
  void setTextWrap(bool) {}
  void setTextColor(int) {}
  void setCursor(int16_t, int16_t) {}
  void setRotation(int) {}
  void print(const char* s) { frame.push_back(s); }
  void print(char c) { frame.push_back(std::string(1, c)); }
  void println(const char* s) { frame.push_back(s); }
  void clearDisplay() { frame.clear(); }
  void display() { shown = frame; }
  void getTextBounds(const char* s, int16_t, int16_t, int16_t* x1, int16_t* y1, uint16_t* w, uint16_t* h) {
    *x1 = 0; *y1 = -13; *w = (uint16_t)(strlen(s) * 8); *h = 13; }
  void drawRect(int16_t, int16_t, int16_t, int16_t, int) {}
  void fillRect(int16_t, int16_t, int16_t, int16_t, int) {}
  void drawRoundRect(int16_t, int16_t, int16_t, int16_t, int16_t, int) {}
  void fillRoundRect(int16_t, int16_t, int16_t, int16_t, int16_t, int) {}
  void drawFastHLine(int16_t, int16_t, int16_t, int) {}
};
