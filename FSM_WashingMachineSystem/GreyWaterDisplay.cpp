#include "GreyWaterSystem.h"
#include "GreyWaterOutputs.h"

// OLED display. State logic only sets the text (displaySetStatus /
// displayShowNotice); displayUpdate(), called every loop() pass, does all the
// drawing, only when something visible changed. Runs in loop() only.

Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
static bool displayOk = true;        // false if the OLED did not answer at start-up

// ── OLED display ───────────────────────────────────────────────────────────
// Layout on the 128x32 screen:
//
//   y  0-8   header : step/title (built-in 6x8 font) | tank level | wash valve
//   y 10     separator line
//   y 12-31  message: FreeSans 9pt, centred when it fits, scrolled when not
//
//   +--------------------------------------------------+
//   |1/4 FILL                          [##]  [W]       |
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
#define MSG_BASELINE     27
#define TANK_ICON_X      100
#define WASH_ICON_X      115
#define ICON_W           13
#define ICON_H           9

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

void displayShowNotice(const char* title, const char* message, unsigned long durationMs) {
  setText(noticeTitle, sizeof(noticeTitle), title);
  setText(noticeMessage, sizeof(noticeMessage), message);
  noticeStart    = millis();
  noticeDuration = durationMs;
  displayDirty   = true;
}

void displayClearNotice() {
  if (noticeDuration) displayDirty = true;
  noticeDuration = 0;
}

// Tank icon: outline, filled bottom-up according to the level sensors.
static void drawTankIcon(WaterLevel level) {
  oled.drawRect(TANK_ICON_X, 0, ICON_W, ICON_H, WHITE);
  int16_t fillH = (level == high) ? ICON_H - 2 : (level == medium) ? (ICON_H - 2) / 2 : 0;
  if (fillH > 0) {
    oled.fillRect(TANK_ICON_X + 1, ICON_H - 1 - fillH, ICON_W - 2, fillH, WHITE);
  }
}

// Wash icon: "W" in an outline when the valve is closed, inverted when open.
static void drawWashIcon(bool open) {
  if (open) {
    oled.fillRoundRect(WASH_ICON_X, 0, ICON_W, ICON_H, 2, WHITE);
    oled.setTextColor(BLACK);
  } else {
    oled.drawRoundRect(WASH_ICON_X, 0, ICON_W, ICON_H, 2, WHITE);
    oled.setTextColor(WHITE);
  }
  oled.setCursor(WASH_ICON_X + 4, HEADER_TEXT_Y);
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
  static int16_t scrollX       = 0;
  static unsigned long scrollStart = 0;

  if (!displayOk) return;   // no screen: everything else keeps running
  if (!force && millis() - lastFrame < DISPLAY_FRAME_MS) return;
  if (force) displayDirty = true;
  lastFrame = millis();

  // A notice overrides the status until it expires.
  bool noticeActive = noticeDuration && (millis() - noticeStart < noticeDuration);
  if (!noticeActive && noticeDuration) { noticeDuration = 0; displayDirty = true; }
  const char* title   = noticeActive ? noticeTitle   : statusTitle;
  const char* message = noticeActive ? noticeMessage : statusMessage;

  // New message: measure it once and restart the scroll from the left.
  if (setText(shownMessage, sizeof(shownMessage), message)) {
    oled.setFont(&FreeSans9pt7b);
    oled.setTextSize(1);
    oled.setTextWrap(false);
    int16_t x1, y1;
    uint16_t w, h;
    oled.getTextBounds(message, 0, MSG_BASELINE, &x1, &y1, &w, &h);
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
  oled.setFont(&FreeSans9pt7b);
  oled.setTextColor(WHITE);
  if (!scrolling) {
    oled.setCursor((OLED_WIDTH - msgWidth) / 2 - msgOffsetX, MSG_BASELINE);
    oled.print(message);
  } else {
    // Draw the text twice so the start follows the end around seamlessly.
    int16_t loopLen = msgWidth + SCROLL_GAP_PX;
    oled.setCursor(-scrollX - msgOffsetX, MSG_BASELINE);
    oled.print(message);
    oled.setCursor(-scrollX + loopLen - msgOffsetX, MSG_BASELINE);
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

