// Large screen font (3.14): word wrap and size choice, ScreenWrap.h.
#include "../ScreenWrap.h"
#include <stdio.h>
#include <string>

static int passed = 0, failed = 0;
#define CHECK(cond, ...) do { if (cond) passed++; else { failed++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// The lines of one text joined by '/', or "!" + what fitted if it did not fit.
static std::string wrap(const char* text, uint8_t cols, uint8_t maxLines, bool split) {
  WrapLines w; w.count = 0;
  bool ok = screenWrapAppend(w, text, cols, maxLines, split);
  std::string out = ok ? "" : "!";
  for (uint8_t i = 0; i < w.count; i++) { if (i) out += "/"; out += w.line[i]; }
  return out;
}
static uint8_t sizeOf(const char* m, uint8_t* first = nullptr, uint8_t* rest = nullptr) {
  uint8_t f = 0, r = 0, s = screenMessageSize(m, &f, &r);
  if (first) *first = f;
  if (rest) *rest = r;
  return s;
}

int main() {
  printf("W1 wrap at word boundaries, 10 characters a line\n");
  CHECK(wrap("Start washer", 10, 3, false) == "Start/washer", "Start washer -> %s", wrap("Start washer", 10, 3, false).c_str());
  CHECK(wrap("Push START", 10, 3, false) == "Push START", "a line of exactly 10 characters was wrapped: %s", wrap("Push START", 10, 3, false).c_str());
  CHECK(wrap("125 min left", 10, 3, false) == "125 min/left", "125 min left -> %s", wrap("125 min left", 10, 3, false).c_str());
  CHECK(wrap("a b c d e f", 10, 3, false) == "a b c d e/f", "short words not packed: %s", wrap("a b c d e f", 10, 3, false).c_str());
  CHECK(wrap("  two   spaces ", 10, 3, false) == "two spaces", "extra spaces kept: \"%s\"", wrap("  two   spaces ", 10, 3, false).c_str());
  CHECK(wrap("", 10, 3, false) == "", "an empty text made a line");
  { WrapLines w; w.count = 0; screenWrapAppend(w, "Tank full", 10, 3, false); screenWrapAppend(w, "End 18:42", 10, 3, false);
    CHECK(w.count == 2 && !strcmp(w.line[0], "Tank full") && !strcmp(w.line[1], "End 18:42"), "a second part did not start on a new line"); }

  printf("W2 what does not fit is reported, never cut silently\n");
  CHECK(wrap("Enjuagando...", 10, 3, false) == "!", "a 13-character word accepted at 10 a line: %s", wrap("Enjuagando...", 10, 3, false).c_str());
  CHECK(wrap("one two three four five six seven", 10, 3, false)[0] == '!', "four lines accepted where three fit");
  CHECK(wrap("192.168.101.29 and more", 21, 6, true) == "192.168.101.29 and/more", "size 1 wrap: %s", wrap("192.168.101.29 and more", 21, 6, true).c_str());
  CHECK(wrap("abcdefghijklmnopqrstuvwxyz0123", 21, 6, true) == "abcdefghijklmnopqrstu/vwxyz0123", "a long word not split at 21: %s",
        wrap("abcdefghijklmnopqrstuvwxyz0123", 21, 6, true).c_str());
  { WrapLines w; w.count = 0;
    bool ok = screenWrapAppend(w, "abcdefghijklmnopqrstu abcdefghijklmnopqrstu abcdefghijklmnopqrstu abcdefghijklmnopqrstu abcdefghijklmnopqrstu abcdefghijklmnopqrstu x", 21, 6, true);
    CHECK(!ok && w.count == 6, "a seventh line was written past the buffer (count %d)", w.count);
    for (uint8_t i = 0; i < w.count; i++) CHECK(strlen(w.line[i]) <= 21, "line %d longer than 21 characters", i); }

  printf("W3 size 2 when the message fits three lines of 10, size 1 for that message only otherwise\n");
  uint8_t f, r;
  CHECK(sizeOf("Start washer|Push START", &f, &r) == 2 && f == 2 && r == 1, "Start washer|Push START: size %d, lines %d+%d", sizeOf("Start washer|Push START"), f, r);
  CHECK(sizeOf("Try again", &f, &r) == 2 && f == 1 && r == 0, "Try again: lines %d+%d", f, r);
  CHECK(sizeOf("Washing...|125 min left", &f, &r) == 2 && f == 1 && r == 2, "Washing...|125 min left: lines %d+%d", f, r);
  CHECK(sizeOf("Start washer|Press START") == 1, "four lines shown at size 2");
  CHECK(sizeOf("Wi-Fi|MyHomeNetwork5G") == 1, "a 15-character word shown at size 2");
  CHECK(sizeOf("", &f, &r) == 2 && f == 0 && r == 0, "an empty message: lines %d+%d", f, r);
  // More than two parts: the first stays, the others take turns, so the longest of them counts.
  CHECK(sizeOf("One|Two|Three", &f, &r) == 2 && f == 1 && r == 1, "One|Two|Three: lines %d+%d", f, r);
  CHECK(sizeOf("Tap water|Tank is low|125 min left", &f, &r) == 2 && f == 1 && r == 2, "three parts: lines %d+%d", f, r);
  CHECK(sizeOf("Saving water|Tank is low|x", &f, &r) == 1, "2 + 2 lines shown at size 2");
  // Size 1: up to six lines of 21 characters.
  CHECK(sizeOf("This message is far too long for the large letters|192.168.101.29", &f, &r) == 1 && f == 3 && r == 1, "long message: lines %d+%d", f, r);
  { std::string longest(SCREEN_MSG_LEN - 1, 'x');             // the longest message the display keeps
    CHECK(sizeOf(longest.c_str(), &f, &r) == 1 && f + r <= SCREEN_SMALL_LINES, "95 characters: lines %d+%d", f, r); }

  printf("W4 parts of a message\n");
  char part[SCREEN_MSG_LEN];
  CHECK(screenPageCount("a|b|c") == 3 && screenPageCount("a") == 1, "part count");
  screenPageText(part, sizeof(part), "Tank low|Tap water|25 min left", 1); CHECK(!strcmp(part, "Tap water"), "part 1 is \"%s\"", part);
  screenPageText(part, sizeof(part), "Tank low", 3);                        CHECK(!strcmp(part, ""), "a missing part is \"%s\"", part);
  screenPageText(part, 5, "Tank low|x", 0);                                 CHECK(!strcmp(part, "Tank"), "a short buffer got \"%s\"", part);

  printf("%d checks passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
