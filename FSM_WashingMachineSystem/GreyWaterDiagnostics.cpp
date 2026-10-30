#include "GreyWaterDiagnostics.h"
#include "GreyWaterSystem.h"
#include "GreyWaterSettings.h"
#include "GreyWaterNet.h"
#include <time.h>
#include <stdarg.h>
#include <esp_system.h>

// Event log, loop timing, buzzer and restart reason: things that help people
// see what the controller did. Everything here runs in loop() only.

// ── Event log (Service tab) ────────────────────────────────────────────────
struct EventEntry { uint32_t epoch; uint32_t uptimeS; char text[56]; };
static EventEntry events[EVENT_LOG_SIZE];
static uint8_t    eventHead = 0, eventsStored = 0;

void logEvent(const char* fmt, ...) {
  EventEntry& e = events[eventHead];
  va_list args; va_start(args, fmt); vsnprintf(e.text, sizeof(e.text), fmt, args); va_end(args);
  e.epoch   = timeIsValid() ? (uint32_t)time(nullptr) : 0;
  e.uptimeS = millis() / 1000UL;
  eventHead = (eventHead + 1) % EVENT_LOG_SIZE;
  if (eventsStored < EVENT_LOG_SIZE) eventsStored++;
  DEBUG_PRINT(e.text);
}
int eventCount() { return eventsStored; }
bool eventAt(int i, uint32_t& epoch, uint32_t& uptimeS, const char*& text) {
  if (i < 0 || i >= eventsStored) return false;
  const EventEntry& e = events[(eventHead + EVENT_LOG_SIZE - 1 - i) % EVENT_LOG_SIZE];
  epoch = e.epoch; uptimeS = e.uptimeS; text = e.text;
  return true;
}

// ── Loop timing (Service tab) ──────────────────────────────────────────────
// Measures the time between loop() passes. Valve, pump and button checks run
// once per pass, so a long pass delays them; the Service tab shows the worst.
static uint32_t loopLastUs = 0, loopMaxUs = 0, loopMinuteMaxUs = 0, loopLastMinuteMaxUs = 0;
static uint32_t loopMinuteStart = 0, slowLoggedAt = 0;
static bool     slowLogged = false;

void loopTimingMark() {
  uint32_t nowUs = micros(), nowMs = millis();
  if (loopLastUs != 0) {
    uint32_t dt = nowUs - loopLastUs;
    if (dt > loopMaxUs) loopMaxUs = dt;
    if (dt > loopMinuteMaxUs) loopMinuteMaxUs = dt;
    if (dt / 1000UL >= SLOW_LOOP_MS && (!slowLogged || nowMs - slowLoggedAt >= 60000UL)) {
      slowLogged = true; slowLoggedAt = nowMs;
      logEvent("Slow loop pass: %lu ms", (unsigned long)(dt / 1000UL));
    }
  }
  if (nowMs - loopMinuteStart >= 60000UL) {
    loopMinuteStart = nowMs; loopLastMinuteMaxUs = loopMinuteMaxUs; loopMinuteMaxUs = 0;
  }
  loopLastUs = nowUs;
}
uint32_t loopMaxMs()           { return loopMaxUs / 1000UL; }
uint32_t loopMaxLastMinuteMs() { return loopLastMinuteMaxUs / 1000UL; }

// ── Optional buzzer ────────────────────────────────────────────────────────
// Patterns are on/off times in ms, ending with 0. Non-blocking.
const uint16_t BEEP_DONE[]  = { 150, 120, 150, 0 };
const uint16_t BEEP_ALARM[] = { 500, 250, 500, 250, 500, 0 };
static const uint16_t* beepPattern = nullptr;
static uint8_t         beepStep = 0;
static unsigned long   beepSince = 0;

void buzzerBegin() { pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW); }

void beep(const uint16_t* pattern) {
  if (!cfg.buzzerOn) return;
  beepPattern = pattern; beepStep = 0; beepSince = millis();
  digitalWrite(BUZZER_PIN, HIGH);
}
void buzzerUpdate() {
  if (!beepPattern || millis() - beepSince < beepPattern[beepStep]) return;
  beepStep++; beepSince = millis();
  if (beepPattern[beepStep] == 0) { digitalWrite(BUZZER_PIN, LOW); beepPattern = nullptr; return; }
  digitalWrite(BUZZER_PIN, (beepStep % 2 == 0) ? HIGH : LOW);
}

// ── Restart reason ─────────────────────────────────────────────────────────
const char* resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "Switched on or power cut";
    case ESP_RST_SW:       return "Restarted by software or update";
    case ESP_RST_PANIC:    return "Program crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "Watchdog (program froze)";
    case ESP_RST_BROWNOUT: return "Power dip (brownout)";
    case ESP_RST_EXT:      return "Reset button";
    default:               return "Other";
  }
}

// The same, in the language chosen in Settings (app and alerts; the event log stays English).
const char* resetReasonTextLocal() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return TR("Switched on or power cut", "Encendido o corte de luz");
    case ESP_RST_SW:       return TR("Restarted by software or update", "Reinicio por software o actualización");
    case ESP_RST_PANIC:    return TR("Program crash", "Fallo del programa");
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return TR("Watchdog (program froze)", "Watchdog (el programa se bloqueó)");
    case ESP_RST_BROWNOUT: return TR("Power dip (brownout)", "Bajón de tensión");
    case ESP_RST_EXT:      return TR("Reset button", "Botón de reinicio");
    default:               return TR("Other", "Otro");
  }
}

