#ifndef SCREEN_WRAP_H
#define SCREEN_WRAP_H

// Text layout for the Large screen font (3.14): the built-in 6x8 font at twice
// its size in the blue rows of the screen (rows 16-63), up to three lines of
// 10 characters, each centred. A message that does not fit that way (a word
// longer than 10 characters, more than three lines) is shown at the normal
// size instead, up to six lines of 21 characters, so nothing is cut off.
//
// A message has parts separated by '|' ("Tank full|Ends 18:42"). Every part
// starts on a new line and is wrapped at word boundaries. With more than two
// parts the first stays and the others take turns under it.
//
// Plain C++ with no Arduino calls, so tests/test_screen_wrap.cpp runs it on
// the computer.

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define SCREEN_MSG_LEN      96   // all parts of a message, with the final 0
#define SCREEN_LARGE_SIZE   2    // setTextSize(): letters 10x14 px, lines 16 px
#define SCREEN_LARGE_COLS   10
#define SCREEN_LARGE_LINES  3
#define SCREEN_SMALL_COLS   21   // setTextSize(1): letters 5x7 px, lines 8 px
#define SCREEN_SMALL_LINES  6

struct WrapLines {
  uint8_t count;
  char line[SCREEN_SMALL_LINES][SCREEN_SMALL_COLS + 1];
};

// Parts in a message ("a|b|c" = 3).
static inline uint8_t screenPageCount(const char* m) {
  uint8_t n = 1;
  for (; *m; ++m) if (*m == '|') ++n;
  return n;
}

// Copies part `index` of message m into dst.
static inline void screenPageText(char* dst, size_t size, const char* m, uint8_t index) {
  for (uint8_t i = 0; i < index && *m; ++m) if (*m == '|') ++i;
  size_t o = 0;
  while (*m && *m != '|' && o + 1 < size) dst[o++] = *m++;
  dst[o] = '\0';
}

// Adds `text` to w, wrapped at spaces into lines of at most `cols` characters,
// starting on a new line. A word longer than a line is split if
// `splitLongWords`. Returns false if it does not fit: a word too long to keep
// whole, or more than `maxLines` lines (w then holds what did fit).
static inline bool screenWrapAppend(WrapLines& w, const char* text, uint8_t cols, uint8_t maxLines, bool splitLongWords) {
  char* cur = nullptr;      // the line being filled
  uint8_t len = 0;
  const char* p = text;
  for (;;) {
    while (*p == ' ') ++p;
    if (!*p) return true;
    const char* word = p;
    size_t wl = 0;
    while (p[wl] && p[wl] != ' ') ++wl;
    p += wl;
    if (wl > cols && !splitLongWords) return false;
    while (wl) {
      if (cur && len + 1 + wl <= cols) {        // fits after a space
        cur[len++] = ' ';
        memcpy(cur + len, word, wl);
        len += (uint8_t)wl;
        cur[len] = '\0';
        break;
      }
      if (w.count >= maxLines) return false;
      cur = w.line[w.count++];
      uint8_t take = wl > cols ? cols : (uint8_t)wl;
      memcpy(cur, word, take);
      len = take;
      cur[len] = '\0';
      word += take;
      wl -= take;
    }
  }
}

// Lines part `page` of m needs, or 0xFF if it does not fit.
static inline uint8_t screenPageLines(const char* m, uint8_t page, uint8_t cols, uint8_t maxLines, bool splitLongWords) {
  char text[SCREEN_MSG_LEN];
  WrapLines w;
  w.count = 0;
  screenPageText(text, sizeof(text), m, page);
  return screenWrapAppend(w, text, cols, maxLines, splitLongWords) ? w.count : 0xFF;
}

// The text size for message m: SCREEN_LARGE_SIZE if the first part and the
// longest of the other parts fit in three lines of 10 characters, otherwise 1.
// Sets the lines of the first part and of the longest other part.
static inline uint8_t screenMessageSize(const char* m, uint8_t* firstLines, uint8_t* restLines) {
  const uint8_t pages = screenPageCount(m);
  for (uint8_t size = SCREEN_LARGE_SIZE; size >= 1; size -= SCREEN_LARGE_SIZE - 1) {
    const bool    large = size == SCREEN_LARGE_SIZE;
    const uint8_t cols  = large ? SCREEN_LARGE_COLS : SCREEN_SMALL_COLS;
    const uint8_t lines = large ? SCREEN_LARGE_LINES : SCREEN_SMALL_LINES;
    uint8_t first = screenPageLines(m, 0, cols, lines, !large), rest = 0;
    bool fits = first != 0xFF;
    for (uint8_t i = 1; i < pages && fits; ++i) {
      uint8_t n = screenPageLines(m, i, cols, lines, !large);
      if (n == 0xFF) fits = false; else if (n > rest) rest = n;
    }
    if (fits && first + rest <= lines) { *firstLines = first; *restLines = rest; return size; }
    if (!large) {            // longer than the screen even at size 1: show what fits
      *firstLines = first == 0xFF ? lines : first;
      *restLines  = lines - *firstLines;
    }
  }
  return 1;
}

#endif
