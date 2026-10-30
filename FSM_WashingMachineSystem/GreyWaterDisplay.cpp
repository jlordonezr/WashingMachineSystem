#include "GreyWaterSystem.h"
#include "GreyWaterOutputs.h"
#include "BootAnimation.h"
#include "FontHelvBold12.h"
#include "FontHelvBold14.h"
#include <math.h>

// OLED display. State logic only sets the text (displaySetStatus /
// displayShowNotice); displayUpdate(), called every loop() pass, does all the
// drawing, only when something visible changed. Runs in loop() only.

Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
static bool displayOk = true;        // false if the OLED did not answer at start-up

// ── OLED display ───────────────────────────────────────────────────────────
// Layout on the 128x32 screen:
//
//   y  0-9   header : step/title (built-in 6x8 font, x 0-95) | tank level (x 98-114) | wash valve (x 117-127)
//   y 10     separator line
//   y 12-31  message: Helvetica Bold pixel font, 14 px when the message fits,
//            else 12 px; centred when it fits, scrolled when not
//
//   +--------------------------------------------------+
//   |1/4 FILL                            [###] [W]     |
//   |--------------------------------------------------|
//   |            Using tank water                      |
//   +--------------------------------------------------+
//
// State logic only *sets* the text (displaySetStatus / displayShowNotice);
// displayUpdate(), called every loop() pass, does all drawing. That keeps the
// scroll smooth (it no longer depends on the 250 ms state timer) and only
// sends a frame over I2C when something on screen actually changed.

#define DISP_TITLE_LEN   22
#define DISP_MSG_LEN     48
#define HEADER_TEXT_Y    1
#define SEPARATOR_Y      10
#define MSG_BASELINE_14  28    // 14 px capitals on rows 14-27, descenders to row 31
#define MSG_BASELINE_12  27    // 12 px capitals on rows 15-26, descenders to row 30
#define TANK_ICON_X      98    // titles are at most 16 characters (96 px), see tools/check_screen_text.py
#define TANK_ICON_W      17
#define WASH_ICON_X      117
#define WASH_ICON_W      11
#define ICON_H           10    // full header height, down to the separator

static char statusTitle[DISP_TITLE_LEN]   = "";
static char statusMessage[DISP_MSG_LEN]   = "";
static char statusDetail[64]              = "";   // app sentence, may hold UTF-8 (Spanish)
static char noticeTitle[DISP_TITLE_LEN]   = "";
static char noticeMessage[DISP_MSG_LEN]   = "";
static unsigned long noticeStart    = 0;
static unsigned long noticeDuration = 0;   // 0 = no notice
static bool displayDirty = true;

bool displayBegin() {
  displayOk = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
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

void displaySetStatus(const char* title, const char* message, const char* detail) {
  setText(statusDetail, sizeof(statusDetail), detail ? detail : message);
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
  float flyY = 7.0f + 5.0f * sinf(intro.frame * 0.21f) + 2.0f * sinf(intro.frame * 0.53f);

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
  noticeDuration = durationMs;
  displayDirty   = true;
}

void displayClearNotice() {
  if (noticeDuration || introActive) displayDirty = true;
  noticeDuration = 0;
  introActive    = false;   // Start or a tank wash: straight to the cycle screens
  deferredNotice = false;
}

// Tank icon: outline, filled bottom-up according to the level sensors.
// Half-way ticks on both sides mark the low float, so an empty tank still
// reads as a tank and "half" is easy to tell from "full".
static void drawTankIcon(WaterLevel level) {
  const int16_t innerH = ICON_H - 2;
  oled.drawRect(TANK_ICON_X, 0, TANK_ICON_W, ICON_H, WHITE);
  int16_t fillH = (level == high) ? innerH : (level == medium) ? innerH / 2 : 0;
  if (fillH > 0) {
    oled.fillRect(TANK_ICON_X + 1, ICON_H - 1 - fillH, TANK_ICON_W - 2, fillH, WHITE);
  }
  if (level != high) {
    int16_t tickY = ICON_H - 1 - innerH / 2 - 1;   // just above the half-way fill line
    oled.drawFastHLine(TANK_ICON_X + 1, tickY, 2, WHITE);
    oled.drawFastHLine(TANK_ICON_X + TANK_ICON_W - 3, tickY, 2, WHITE);
  }
}

// Wash icon: "W" in an outline when the valve is closed, inverted when open.
static void drawWashIcon(bool open) {
  if (open) {
    oled.fillRoundRect(WASH_ICON_X, 0, WASH_ICON_W, ICON_H, 2, WHITE);
    oled.setTextColor(BLACK);
  } else {
    oled.drawRoundRect(WASH_ICON_X, 0, WASH_ICON_W, ICON_H, 2, WHITE);
    oled.setTextColor(WHITE);
  }
  oled.setCursor(WASH_ICON_X + 3, HEADER_TEXT_Y);   // 5 px glyph centred in the 11 px box, on the title line
  oled.print('W');
  oled.setTextColor(WHITE);
}

const char* displayGetTitle()   { return statusTitle; }
const char* displayGetMessage() { return statusMessage; }
const char* displayGetDetail()  { return statusDetail; }

void displayUpdate(bool force) {
  static unsigned long lastFrame   = 0;
  static char shownTitle[DISP_TITLE_LEN] = "";
  static char shownMessage[DISP_MSG_LEN] = "\x01";  // never matches: forces first measure
  static WaterLevel shownLevel = low;
  static bool shownWash        = false;
  static int16_t msgWidth      = 0;
  static int16_t msgOffsetX    = 0;
  static const GFXfont* msgFont = &HelvBold14;
  static int16_t msgBaseline   = MSG_BASELINE_14;
  static int16_t scrollX       = 0;
  static unsigned long scrollStart = 0;

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

  // New message: pick the size and measure it once, restart the scroll from
  // the left. 14 px when it fits the width, else 12 px (every screen text fits
  // in 12 px, see tools/check_screen_text.py).
  if (setText(shownMessage, sizeof(shownMessage), message)) {
    oled.setTextSize(1);
    oled.setTextWrap(false);
    int16_t x1, y1;
    uint16_t w, h;
    msgFont = &HelvBold14;  msgBaseline = MSG_BASELINE_14;
    oled.setFont(msgFont);
    oled.getTextBounds(message, 0, msgBaseline, &x1, &y1, &w, &h);
    if (w > OLED_WIDTH) {
      msgFont = &HelvBold12;  msgBaseline = MSG_BASELINE_12;
      oled.setFont(msgFont);
      oled.getTextBounds(message, 0, msgBaseline, &x1, &y1, &w, &h);
    }
    msgWidth    = w;
    msgOffsetX  = x1;
    scrollX     = 0;
    scrollStart = millis();
    displayDirty = true;
  }
  if (setText(shownTitle, sizeof(shownTitle), title)) displayDirty = true;

  WaterLevel level = tankLevel();
  bool washOpen = outputsWashOpen();
  if (level != shownLevel)              { shownLevel = level;           displayDirty = true; }
  if (washOpen != shownWash)            { shownWash = washOpen;          displayDirty = true; }

  bool scrolling = msgWidth > OLED_WIDTH;
  bool moving    = scrolling && (millis() - scrollStart >= SCROLL_PAUSE_MS);
  if (!displayDirty && !moving) return;   // nothing changed: skip the I2C transfer
  displayDirty = false;

  oled.clearDisplay();
  oled.setTextWrap(false);

  // Header
  oled.setFont();          // built-in 6x8 font
  oled.setTextSize(1);
  oled.setTextColor(WHITE);
  oled.setCursor(0, HEADER_TEXT_Y);
  oled.print(title);
  drawTankIcon(level);
  drawWashIcon(washOpen);
  oled.drawFastHLine(0, SEPARATOR_Y, OLED_WIDTH, WHITE);

  // Message
  oled.setFont(msgFont);
  oled.setTextColor(WHITE);
  if (!scrolling) {
    oled.setCursor((OLED_WIDTH - msgWidth) / 2 - msgOffsetX, msgBaseline);
    oled.print(message);
  } else {
    // Draw the text twice so the start follows the end around seamlessly.
    int16_t loopLen = msgWidth + SCROLL_GAP_PX;
    oled.setCursor(-scrollX - msgOffsetX, msgBaseline);
    oled.print(message);
    oled.setCursor(-scrollX + loopLen - msgOffsetX, msgBaseline);
    oled.print(message);

    if (moving) {
      scrollX += SCROLL_SPEED_PX;
      if (scrollX >= loopLen) {       // one full loop done: rest again at the start
        scrollX = 0;
        scrollStart = millis();
      }
    }
  }

  oled.display();
}

