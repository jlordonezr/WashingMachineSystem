// Records what the firmware prints, so tests can read the screen.
#pragma once
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <string>
#include <vector>
#define WHITE 1
#define BLACK 0
#define SSD1306_SWITCHCAPVCC 2
#define SSD1306_SETCONTRAST 0x81
// The real library's begin() does not notice a missing screen; the firmware
// probes the I2C address first (Wire.h: oledAddress).
struct Adafruit_SSD1306 {
  std::vector<std::string> frame, shown;
  std::vector<int> frameY, shownY;      // the row (cursor y) each text was printed at
  std::vector<int> frameX, shownX;      // ... its column (cursor x)
  std::vector<int> frameSize, shownSize;   // ... and its text size (setTextSize)
  int height = 0, beginAddress = 0, cursorX = 0, cursorY = 0, textSize = 1;
  Adafruit_SSD1306(int, int h, TwoWire*, int) : height(h) {}
  bool begin(int, int address) { beginAddress = address; return true; }
  void ssd1306_command(uint8_t) {}
  bool fontSet = false;
  void setFont(const GFXfont* f = nullptr) { fontSet = f != nullptr; }
  void setTextSize(int s) { textSize = s; }
  void setTextWrap(bool) {}
  void setTextColor(int) {}
  void setCursor(int16_t x, int16_t y) { cursorX = x; cursorY = y; }
  void setRotation(int) {}
  void print(const char* s) { frame.push_back(s); frameY.push_back(cursorY); frameX.push_back(cursorX); frameSize.push_back(textSize); }
  void print(char c) { print(std::string(1, c).c_str()); }
  void println(const char* s) { print(s); }
  void clearDisplay() { frame.clear(); frameY.clear(); frameX.clear(); frameSize.clear(); }
  void display() { shown = frame; shownY = frameY; shownX = frameX; shownSize = frameSize; displays++; }
  int displays = 0;                     // frames sent
  void getTextBounds(const char* s, int16_t, int16_t, int16_t* x1, int16_t* y1, uint16_t* w, uint16_t* h) {
    if (fontSet) { *x1 = 0; *y1 = -13; *w = (uint16_t)(strlen(s) * 8); *h = 13; }
    else { *x1 = 0; *y1 = 0; *w = (uint16_t)(strlen(s) * 6 * textSize); *h = (uint16_t)(8 * textSize); } }   // built-in 6x8 font, as the library
  void drawRect(int16_t, int16_t, int16_t, int16_t, int) {}
  void fillRect(int16_t, int16_t, int16_t, int16_t, int) {}
  void drawRoundRect(int16_t, int16_t, int16_t, int16_t, int16_t, int) {}
  void fillRoundRect(int16_t, int16_t, int16_t, int16_t, int16_t, int) {}
  void drawFastHLine(int16_t, int16_t, int16_t, int) {}
  void drawFastVLine(int16_t, int16_t, int16_t, int) {}
  void drawPixel(int16_t, int16_t, int) {}
  void drawBitmap(int16_t, int16_t, const uint8_t*, int16_t, int16_t, int) {}
};
