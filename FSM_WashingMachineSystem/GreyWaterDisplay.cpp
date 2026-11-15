#include "GreyWaterSystem.h"
#include "GreyWaterOutputs.h"
#include "BootAnimation.h"
#include "FontHyperlegible12.h"
#include "FontPlainSans13.h"
#include "FontTerminal16.h"
#include "FontSchoolbook10.h"
#include <math.h>
#include <ctype.h>

// OLED display. State logic only sets the text (displaySetStatus /
// displayShowNotice); displayUpdate(), called every loop() pass, does all the
// drawing, only when something visible changed. Runs in loop() only.

Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
static bool displayOk = true;        // false if the OLED did not answer at start-up

// ── OLED display ───────────────────────────────────────────────────────────
// The screen (3.12) is a 0.96" 128x64 SSD1306 with two fixed colours: rows
// 0-15 light up yellow, rows 16-63 blue (the glass decides, not the firmware).
// The layout follows the colours:
//
//   y  0-15  yellow, header : step/title (built-in 6x8 font, x 0-71, 12 characters) | tank litres or FULL/EMPTY (x 73-108)
//                            | fill/drain arrow (x 104-108) | tank gauge (x 111-127, 3.13)
//   y 16-63  blue, message  : two lines in the font chosen in Settings (Screen
//            font), one fixed size for every message, each line centred
//
// A message may have several pages separated by '|', for example
// "Tank low|Using tap water|25 min left". One line holds only about 15
// characters, so pages let the screen say what is happening, what it means
// and what to do. The first page stays on the top line; the other pages take
// turns on the bottom line, changing every DISPLAY_PAGE_MS. A message of two
// pages is shown whole and never changes; a message of one page sits alone in
// the middle of the blue area.
//
//   +--------------------------------------------------+
//   |WASH 1 OF 4                    63L ^ [~~~]        |  yellow
//   |                                                  |
//   |                  Tank low                        |  blue
//   |              Using tap water                     |  blue (then "25 min left")
//   +--------------------------------------------------+
//
// State logic only *sets* the text (displaySetStatus / displayShowNotice);
// displayUpdate(), called every loop() pass, does all drawing and only sends a
// frame over I2C when something on screen actually changed.

#define DISP_TITLE_LEN   22
#define DISP_MSG_LEN     96    // all pages of a message
#define HEADER_H         16    // the yellow rows
#define HEADER_TEXT_Y    4     // 8 px letters on rows 4-11, centred in the yellow band
// Message fonts, chosen in Settings -> Screen font (cfg.screenFont). Each font
// has ONE size for every message, so the letters never change size between
// pages or screens: the largest size in which every screen text fits the
// 128 px line (tools/check_screen_text.py fails otherwise). The two baselines
// place the top and the bottom line in the blue area, rows 17-63, clear of the
// yellow rows and of each other; a one-page message is drawn half-way between.
struct ScreenFont { const GFXfont* font; int16_t baseline1, baseline2; };
static const ScreenFont SCREEN_FONTS[SCREEN_FONT_COUNT] = {
  { &Hyperlegible12, 33, 57 },   // 0 hyperlegible (default): capitals 12 px, rows 20-36 and 44-60
  { &PlainSans13,    34, 58 },   // 1 plain sans: capitals 13 px, rows 19-37 and 43-61
  { &Terminal16,     33, 57 },   // 2 terminal bold: capitals 12 px, rows 21-36 and 45-60
  { &Schoolbook10,   33, 57 },   // 3 schoolbook bold: capitals 11 px, rows 21-35 and 45-59
};
#define TITLE_MAX_CHARS  12    // 72 px: titles fit beside the tank litres (tools/check_screen_text.py)
#define TANK_ICON_X      111   // tank gauge: x 111-127, rows 1-14 of the yellow band
#define TANK_ICON_W      17
#define ICON_Y           1
#define ICON_H           14
#define TANK_ARROW_X     104   // fill/drain arrow: x 104-108
#define TANK_TEXT_END    103   // the litres end here (right-aligned); FULL/EMPTY may use the arrow's place

static char statusTitle[DISP_TITLE_LEN]   = "";
static char statusMessage[DISP_MSG_LEN]   = "";
static char statusDetail[96]              = "";   // app sentence, may hold UTF-8 (Spanish)
static char noticeTitle[DISP_TITLE_LEN]   = "";
static char noticeMessage[DISP_MSG_LEN]   = "";
static unsigned long noticeStart    = 0;
static unsigned long noticeDuration = 0;   // 0 = no notice
static bool displayDirty = true;

// True if a device answers at this I2C address.
static bool i2cAnswers(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

// These modules answer at 0x3C or, with the address jumper on the back moved
// ("0x7A"), at 0x3D. The library's begin() does not notice a missing screen,
// so the address is probed first.
bool displayBegin() {
  static const uint8_t ADDRESSES[] = { 0x3C, 0x3D };
  Wire.begin();
  displayOk = false;
  for (uint8_t address : ADDRESSES) {
    if (!i2cAnswers(address)) continue;
    displayOk = oled.begin(SSD1306_SWITCHCAPVCC, address);
    break;
  }
  if (displayOk) {                        // full brightness (the library starts at 0xCF): easier to read from a distance
    oled.ssd1306_command(SSD1306_SETCONTRAST);
    oled.ssd1306_command(0xFF);
  }
  return displayOk;
}
bool displayAvailable() { return displayOk; }

// Copies src into dst (truncating safely) and reports whether it changed.
static bool setText(char* dst, size_t size, const char* src) {
  if (strncmp(dst, src, size - 1) == 0) return false;
  strncpy(dst, src, size - 1);
  dst[size - 1] = '\0';
  return true;
}

// Pages in a message ("a|b|c" = 3).
static uint8_t pageCount(const char* m) {
  uint8_t n = 1;
  for (; *m; ++m) if (*m == '|') ++n;
  return n;
}

// Copies page `index` of message m into dst.
static void pageText(char* dst, size_t size, const char* m, uint8_t index) {
  for (uint8_t i = 0; i < index && *m; ++m) if (*m == '|') ++i;
  size_t o = 0;
  while (*m && *m != '|' && o + 1 < size) dst[o++] = *m++;
  dst[o] = '\0';
}

void displaySetStatus(const char* title, const char* message, const char* detail) {
  if (detail) {
    setText(statusDetail, sizeof(statusDetail), detail);
  } else {                       // no app sentence: the pages joined by spaces
    char joined[sizeof(statusDetail)];
    strlcpy(joined, message, sizeof(joined));
    for (char* c = joined; *c; ++c) if (*c == '|') *c = ' ';
    setText(statusDetail, sizeof(statusDetail), joined);
  }
  bool changed = setText(statusTitle, sizeof(statusTitle), title);
  changed |= setText(statusMessage, sizeof(statusMessage), message);
  if (changed) displayDirty = true;
}

// ── Start-up animation ─────────────────────────────────────────────────────
// After power-on a Border Collie chases a butterfly across the whole screen
// (about 4 s), then the start-up notice ("System ready") follows. Notices that
// arrive meanwhile (Wi-Fi address, setup key, "Interrupted") are held and shown
// after it. Start or a tank wash (displayClearNotice) ends it at once.
static bool introActive = false;
static char introNextTitle[DISP_TITLE_LEN] = "";
static char introNextMessage[DISP_MSG_LEN] = "";
static unsigned long introNextMs = 0;
static bool deferredNotice = false;
static char deferredTitle[DISP_TITLE_LEN] = "";
static char deferredMessage[DISP_MSG_LEN] = "";
static unsigned long deferredMs = 0;

#define INTRO_SPEED_PX   2.2f   // dog speed per frame (55 px/s at 25 fps)
#define INTRO_STALK_X    66.0f  // the collie starts to stalk here...
#define INTRO_STALK_FRAMES 28   // ...for this many frames (1.1 s)
#define INTRO_END_X      (-(COLLIE_W + 2))

static struct {
  float dogX, flyX, flyBoost;
  uint16_t frame, stalkT;
  bool stalking, stalkDone;
} intro;

void displayStartIntro(const char* title, const char* message, unsigned long durationMs) {
  if (!displayOk) { displayShowNotice(title, message, durationMs); return; }
  setText(introNextTitle, sizeof(introNextTitle), title);
  setText(introNextMessage, sizeof(introNextMessage), message);
  introNextMs = durationMs;
  intro.dogX = OLED_WIDTH + 4;  intro.flyX = OLED_WIDTH - 6;  intro.flyBoost = 0;
  intro.frame = 0;  intro.stalkT = 0;  intro.stalking = false;  intro.stalkDone = false;
  introActive = true;
}

static int16_t roundPx(float v) { return (int16_t)floorf(v + 0.5f); }

// One frame of the chase. Returns false when both have left the screen.
static bool drawIntroFrame() {
  const float v = INTRO_SPEED_PX;
  intro.frame++;
  intro.dogX -= v;
  // The butterfly flutters ahead, letting the dog close in, then pulling away.
  float gap  = intro.dogX - intro.flyX;
  float want = 18.0f + 8.0f * sinf(intro.frame * 0.09f);
  float flyStep = v * (gap < want ? 1.35f : 0.8f);
  intro.flyX -= flyStep;
  // 128x64 (3.12): the collie runs along the bottom of the blue rows; the
  // butterfly flutters from its head height up into the yellow band (rows 4-46;
  // on the 128x32 screen it was rows 0-14, the collie's head height).
  float flyY = 19.0f + 11.0f * sinf(intro.frame * 0.21f) + 3.0f * sinf(intro.frame * 0.53f);

  // Gallop in; the butterfly hovers and the collie drops into its stalk and
  // creeps; then the butterfly darts off and the collie sprints after it.
  if (intro.dogX <= INTRO_STALK_X && !intro.stalking && !intro.stalkDone) {
    intro.stalking = true;
    intro.stalkT = 0;
  }
  if (intro.stalking) {
    intro.dogX += v - v * 0.18f;                                  // creep instead of run
    intro.stalkT++;
    intro.flyX += (intro.dogX - 34.0f - intro.flyX) * 0.08f - flyStep;   // hover in front of the dog
    if (intro.stalkT > INTRO_STALK_FRAMES - 1) { intro.stalking = false; intro.stalkDone = true; intro.flyBoost = 10.0f; }
  }
  if (intro.flyBoost > 0) { intro.flyX -= v * 0.9f; intro.flyBoost -= 0.2f; }

  oled.clearDisplay();
  int16_t groundShift = (int16_t)floorf(intro.frame * v);           // dotted ground moving with the dog
  for (int16_t x = 0; x < OLED_WIDTH; x++) {
    if ((x + groundShift) % 5 < 3) oled.drawPixel(x, OLED_HEIGHT - 1, WHITE);
  }
  int16_t fy = roundPx(flyY) - 1;
  oled.drawBitmap(roundPx(intro.flyX), fy < 0 ? 0 : fy, flyFrames[(intro.frame / 2) % 4], FLY_W, FLY_H, WHITE);
  uint8_t dogFrame = intro.stalking ? COLLIE_GALLOP_FRAMES + (intro.stalkT / 7) % 2
                                    : (uint8_t)((int)floorf(intro.frame * v / 4.0f) % COLLIE_GALLOP_FRAMES);
  oled.drawBitmap(roundPx(intro.dogX), OLED_HEIGHT - COLLIE_H, collieFrames[dogFrame], COLLIE_W, COLLIE_H, WHITE);
  oled.display();
  return intro.dogX >= INTRO_END_X;
}

void displayShowNotice(const char* title, const char* message, unsigned long durationMs) {
  if (introActive) {             // keep it for after the start-up animation
    setText(deferredTitle, sizeof(deferredTitle), title);
    setText(deferredMessage, sizeof(deferredMessage), message);
    deferredMs = durationMs;
    deferredNotice = true;
    return;
  }
  setText(noticeTitle, sizeof(noticeTitle), title);
  setText(noticeMessage, sizeof(noticeMessage), message);
  noticeStart    = millis();
  uint8_t n = pageCount(message);             // the first page stays; the others take turns under it
  unsigned long allPages = (unsigned long)(n > 1 ? n - 1 : 1) * DISPLAY_PAGE_MS;
  noticeDuration = durationMs > allPages ? durationMs : allPages;   // long enough to show every page once
  displayDirty   = true;
}

void displayClearNotice() {
  if (noticeDuration || introActive) displayDirty = true;
  noticeDuration = 0;
  introActive    = false;   // Start or a tank wash: straight to the cycle screens
  deferredNotice = false;
}

// Tank gauge (3.13): the outline filled from the bottom to the estimated
// litres (TankVolume.h), eased so it rises and falls smoothly, with a ripple
// on the surface while water moves. Beside it: the litres ("63L") and an
// arrow, up while the tank fills and down while it drains; FULL when the high
// float is wet, EMPTY when the low float is dry (nothing the pump can use).
// A title longer than TITLE_MAX_CHARS (the Wi-Fi network name, an IP address)
// keeps its full width: the litres and the arrow are left out on that screen.
struct TankGauge { int16_t fill; int8_t trend; uint8_t phase; char label[8]; };

static void tankGaugeState(TankGauge& g, float litres) {
  const int16_t innerH = ICON_H - 2;
  g.fill = (int16_t)floorf(litres / TANK_CAPACITY_L * innerH + 0.5f);
  if (g.fill < 0) g.fill = 0;
  if (g.fill > innerH) g.fill = innerH;
  g.trend = tankTrend();
  if (tankFull()) { strlcpy(g.label, TRS("FULL", "LLENO"), sizeof(g.label)); g.trend = 0; }
  else if (tankEmptyFloat() && g.trend <= 0) { strlcpy(g.label, TRS("EMPTY", "VACIO"), sizeof(g.label)); g.trend = 0; }
  else snprintf(g.label, sizeof(g.label), "%dL", (int)floorf(litres + 0.5f));
  g.phase = g.trend ? (uint8_t)((millis() / (TANK_ANIM_MS * 2)) % 4) : 0;
}

static void drawTankGauge(const TankGauge& g, bool withText) {
  const int16_t innerH = ICON_H - 2, innerW = TANK_ICON_W - 2, x0 = TANK_ICON_X + 1;
  oled.drawRect(TANK_ICON_X, ICON_Y, TANK_ICON_W, ICON_H, WHITE);
  if (g.fill > 0) {
    int16_t top = ICON_Y + ICON_H - 1 - g.fill;
    oled.fillRect(x0, top, innerW, g.fill, WHITE);
    if (g.trend && g.fill < innerH) {                 // ripple: troughs in the surface, crests just above it
      for (int16_t i = 0; i < innerW; i++) {
        uint8_t w = (uint8_t)(i + (g.trend > 0 ? 4 - g.phase : g.phase)) % 4;
        if (w == 0) oled.drawPixel(x0 + i, top, BLACK);
        if (w == 2) oled.drawPixel(x0 + i, top - 1, WHITE);
      }
    }
  }
  if (!withText) return;
  bool word = !isdigit((unsigned char)g.label[0]);      // FULL / EMPTY: no arrow, so it may use the arrow's place
  int16_t end = word ? TANK_ARROW_X + 5 : TANK_TEXT_END;
  oled.setCursor(end - 6 * (int16_t)strlen(g.label) + 1, HEADER_TEXT_Y);
  oled.print(g.label);
  if (g.trend) {                                         // arrow: a shaft and a head, rows 4-10
    const int16_t ax = TANK_ARROW_X + 2;
    oled.drawFastVLine(ax, HEADER_TEXT_Y, 7, WHITE);
    int16_t tip = g.trend > 0 ? HEADER_TEXT_Y : HEADER_TEXT_Y + 6, d = g.trend > 0 ? 1 : -1;
    oled.drawPixel(ax - 1, tip + d, WHITE);     oled.drawPixel(ax + 1, tip + d, WHITE);
    oled.drawPixel(ax - 2, tip + 2 * d, WHITE); oled.drawPixel(ax + 2, tip + 2 * d, WHITE);
  }
}

const char* displayGetTitle()   { return statusTitle; }
const char* displayGetMessage() { return statusMessage; }
const char* displayGetDetail()  { return statusDetail; }

// One line of the message area: its text, measured once when it changes.
struct MsgLine {
  char text[DISP_MSG_LEN];
  int16_t width, offsetX, scrollX;
  unsigned long scrollStart;
};

// Stores a line's new text and measures it in the current font. Returns true
// if the line changed (or `remeasure`: a new font from Settings).
static bool setLine(MsgLine& line, const char* text, bool remeasure) {
  if (!setText(line.text, sizeof(line.text), text) && !remeasure) return false;
  int16_t x1 = 0, y1;
  uint16_t w = 0, h;
  if (line.text[0]) oled.getTextBounds(line.text, 0, 0, &x1, &y1, &w, &h);
  line.width       = w;
  line.offsetX     = x1;
  line.scrollX     = 0;          // restart the scroll from the left
  line.scrollStart = millis();
  return true;
}

// A line wider than the screen (none today, see tools/check_screen_text.py) scrolls.
static bool lineScrolls(const MsgLine& line) { return line.width > OLED_WIDTH; }
static bool lineMoving(const MsgLine& line)  { return lineScrolls(line) && millis() - line.scrollStart >= SCROLL_PAUSE_MS; }

static void drawLine(MsgLine& line, int16_t baseline) {
  if (!line.text[0]) return;
  if (!lineScrolls(line)) {
    oled.setCursor((OLED_WIDTH - line.width) / 2 - line.offsetX, baseline);
    oled.print(line.text);
    return;
  }
  // Draw the text twice so the start follows the end around seamlessly.
  int16_t loopLen = line.width + SCROLL_GAP_PX;
  oled.setCursor(-line.scrollX - line.offsetX, baseline);
  oled.print(line.text);
  oled.setCursor(-line.scrollX + loopLen - line.offsetX, baseline);
  oled.print(line.text);
  if (lineMoving(line)) {
    line.scrollX += SCROLL_SPEED_PX;
    if (line.scrollX >= loopLen) {    // one full loop done: rest again at the start
      line.scrollX = 0;
      line.scrollStart = millis();
    }
  }
}

void displayUpdate(bool force) {
  static unsigned long lastFrame   = 0;
  static char shownTitle[DISP_TITLE_LEN] = "";
  static char shownMessage[DISP_MSG_LEN] = "\x01";  // never matches: forces first measure
  static char firstPage[DISP_MSG_LEN]    = "";
  static MsgLine lines[2] = { { "\x01", 0, 0, 0, 0 }, { "\x01", 0, 0, 0, 0 } };   // top and bottom line on screen now
  static uint8_t shownFont = 0xFF;      // cfg.screenFont the lines were measured with
  static uint8_t pages = 1, pageIndex = 1;   // pageIndex: the page on the bottom line (1 .. pages-1)
  static unsigned long pageStart = 0;
  static TankGauge shownGauge = { -1, 0, 0, "" };
  static float shownL = -1.0f;          // litres on the gauge, eased toward the estimate
  static unsigned long tankAnimAt = 0;

  if (!displayOk) return;   // no screen: everything else keeps running
  if (!force && millis() - lastFrame < DISPLAY_FRAME_MS) return;
  if (force) displayDirty = true;
  lastFrame = millis();

  if (introActive) {
    if (drawIntroFrame()) return;
    introActive = false;    // animation done: the start-up notice, then any held notice
    displayShowNotice(introNextTitle, introNextMessage, introNextMs);
  }

  // A notice overrides the status until it expires.
  bool noticeActive = noticeDuration && (millis() - noticeStart < noticeDuration);
  if (!noticeActive && deferredNotice) {
    deferredNotice = false;
    displayShowNotice(deferredTitle, deferredMessage, deferredMs);
    noticeActive = true;
  }
  if (!noticeActive && noticeDuration) { noticeDuration = 0; displayDirty = true; }
  const char* title   = noticeActive ? noticeTitle   : statusTitle;
  const char* message = noticeActive ? noticeMessage : statusMessage;

  // New message: the bottom line starts again at the second page, unless only
  // a number in a later page changed (a countdown), so the pages keep their rhythm.
  if (setText(shownMessage, sizeof(shownMessage), message)) {
    char first[DISP_MSG_LEN];
    pageText(first, sizeof(first), message, 0);
    uint8_t n = pageCount(message);
    if (n != pages || strcmp(first, firstPage) != 0) {
      strlcpy(firstPage, first, sizeof(firstPage));
      pages = n;
      pageIndex = 1;
      pageStart = millis();
    }
  }
  if (pages > 2 && millis() - pageStart >= DISPLAY_PAGE_MS) {
    pageIndex = pageIndex + 1 < pages ? pageIndex + 1 : 1;
    pageStart = millis();
  }
  char top[DISP_MSG_LEN], bottom[DISP_MSG_LEN] = "";
  pageText(top, sizeof(top), message, 0);
  if (pages > 1) pageText(bottom, sizeof(bottom), message, pageIndex);

  // New text on a line or a new font from Settings: measure the line once.
  uint8_t fontIndex = cfg.screenFont < SCREEN_FONT_COUNT ? cfg.screenFont : 0;
  bool fontChanged = fontIndex != shownFont;
  shownFont = fontIndex;
  const ScreenFont& sf = SCREEN_FONTS[shownFont];
  oled.setTextSize(1);
  oled.setTextWrap(false);
  oled.setFont(sf.font);
  if (setLine(lines[0], top, fontChanged))    displayDirty = true;
  if (setLine(lines[1], bottom, fontChanged)) displayDirty = true;
  if (setText(shownTitle, sizeof(shownTitle), title)) displayDirty = true;

  // Tank gauge: one step every TANK_ANIM_MS (so at most 10 redraws a second for it).
  if (shownL < 0 || millis() - tankAnimAt >= TANK_ANIM_MS) {
    tankAnimAt = millis();
    float target = tankLitres();
    shownL = shownL < 0 ? target : shownL + (target - shownL) * 0.3f;
    if (fabsf(target - shownL) < 0.05f) shownL = target;
    TankGauge g;
    tankGaugeState(g, shownL);
    if (g.fill != shownGauge.fill || g.trend != shownGauge.trend || g.phase != shownGauge.phase ||
        strcmp(g.label, shownGauge.label) != 0) {
      shownGauge = g;
      displayDirty = true;
    }
  }

  bool moving = lineMoving(lines[0]) || lineMoving(lines[1]);
  if (!displayDirty && !moving) return;   // nothing changed: skip the I2C transfer
  displayDirty = false;

  oled.clearDisplay();

  // Header, in the yellow rows
  oled.setFont();          // built-in 6x8 font
  oled.setTextColor(WHITE);
  oled.setCursor(0, HEADER_TEXT_Y);
  oled.print(title);
  drawTankGauge(shownGauge, strlen(title) <= TITLE_MAX_CHARS);

  // Message, in the blue rows: two lines, or one line half-way between them
  oled.setFont(sf.font);
  oled.setTextColor(WHITE);
  if (lines[1].text[0]) {
    drawLine(lines[0], sf.baseline1);
    drawLine(lines[1], sf.baseline2);
  } else {
    drawLine(lines[0], (sf.baseline1 + sf.baseline2) / 2);
  }

  oled.display();
}
