#ifndef GREYWATER_SETTINGS_H
#define GREYWATER_SETTINGS_H

#include <Arduino.h>

// Settings the user can change from the web dashboard. They are stored in
// flash (Preferences) and survive power cuts. The #defines in GreyWaterSystem.h
// are the factory defaults used on first boot and after a version change.
//
// Bump SETTINGS_VERSION / STATS_VERSION whenever a field is added, removed or
// reordered: stored data with a different version is ignored and replaced by
// defaults instead of being read into the wrong fields.
#define SETTINGS_VERSION 7   // 7: adds language (versions 4, 5 and 6 are migrated)

// Language of the screen, the app and the alerts (Settings: Idioma / Language).
// Spanish is 0 so that settings migrated from older versions start in Spanish.
#define LANG_ES 0
#define LANG_EN 1
#define STATS_VERSION    3   // 3: adds typical step times (1 and 2 are migrated)

struct Settings {
  uint16_t version;
  uint32_t washPeriodMs;      // tank-wash valve opens every ... ms (tank-water fill)
  uint32_t washOpenMs;        // ... and stays open this long
  uint32_t drain2DurationMs;  // length of step 4 before returning to Idle
  uint32_t edgeDetectMs;      // flow must last this long to count as "flow seen"
  uint32_t stepAlertMs;       // alert when step 1-3 runs longer than this
  uint16_t tankMaxAgeHours;   // alert when tank water is older than this
  uint32_t manualWashMs;      // safety time limit for the dashboard "Wash tank" button
  float    flowOnLpm;         // flow counts above this (L/min); stopped below it for 2 s
  bool     allowRemoteStart;  // show the Start button on the dashboard
  char     ntfyServer[64];    // e.g. https://ntfy.sh
  char     ntfyTopic[64];     // empty = alerts off
  char     timeZone[48];      // POSIX TZ string, e.g. "<-05>5" for UTC-5
  char     webPassword[33];   // dashboard password (user name: admin)
  char     otaPassword[33];   // password for wireless firmware updates
  // ── added in SETTINGS_VERSION 5; must stay last so version 4 data migrates ──
  float    minFillLitres;     // a fill must take this much before a drain can end the step
  // ── added in SETTINGS_VERSION 6; must stay last so version 5 data migrates ──
  uint8_t  buzzerOn;          // 1 = sound the buzzer on BUZZER_PIN at cycle end and on alarms
  // ── added in SETTINGS_VERSION 7; must stay last so version 6 data migrates ──
  uint8_t  language;          // LANG_ES or LANG_EN
};

extern Settings   cfg;

// Text in the language chosen in Settings: TR("Ready", "Listo").
// Screen texts must be plain ASCII (the screen font has no accents);
// texts for the app and alerts may use UTF-8.
inline const char* TR(const char* en, const char* es) { return cfg.language == LANG_EN ? en : es; }
// The same, for text shown on the OLED: plain ASCII, headers up to 16 characters,
// messages up to 128 px in FreeSans 9 pt. tools/check_screen_text.py checks every TRS().
inline const char* TRS(const char* en, const char* es) { return TR(en, es); }

struct WaterStats {
  uint16_t version;
  float    totalTapL;         // tap water used for fills, all time
  float    totalReusedL;      // tank water reused instead of tap water, all time
  float    totalSavedL;       // rinse water saved into the tank, all time
  uint32_t totalCycles;       // completed cycles, all time
  uint32_t monthKey;          // YYYYMM of the month below (0 = clock not set yet)
  float    monthTapL;
  float    monthReusedL;
  float    monthSavedL;
  uint32_t monthCycles;
  uint32_t tankSinceEpoch;    // when the tank last went from empty to holding water
  // ── added in STATS_VERSION 2 (fields above keep their v1 layout) ──
  uint32_t prevMonthKey;      // YYYYMM of the month before monthKey that had data (0 = none)
  float    prevMonthTapL;
  float    prevMonthReusedL;
  float    prevMonthSavedL;
  uint32_t prevMonthCycles;
  uint32_t lastEndEpoch;      // when the last wash ended (0 = clock was not set)
  uint32_t lastDurationS;     // how long it took, Start to end
  float    lastTapL;
  float    lastReusedL;
  float    lastSavedL;
  uint8_t  lastCompleted;     // 1 = finished normally, 0 = stopped
  uint8_t  lastValid;         // 1 once a wash has been recorded
  // ── added in STATS_VERSION 3; must stay last so version 2 data migrates ──
  uint32_t stepAvgS[3];       // typical duration of steps 1-3 (seconds, running average)
  uint16_t stepSamples[3];    // how many completed steps the average is based on
};

extern WaterStats stats;

void     settingsBegin();                // load settings and stats (defaults if none)
void     settingsDefaults(Settings& s);  // factory values
void     settingsSanitize(Settings& s);  // clamp to safe ranges
bool     settingsSave();
bool     statsRollMonth();               // start a new month if the calendar month changed; true if it did
void     statsSave();                    // writes the history and its backup copy

// Water history protection (2.8). The history is stored twice in flash
// ("stats" and "stats_bak"); a record that cannot be read is kept aside
// ("stats_keep") instead of being overwritten. The app also keeps a copy on
// each phone and can restore it with statsRestore() after a full flash erase.
enum HistorySource { historyEmpty, historyFlash, historyBackup, historyMigrated, historyRestored, historyUnreadable };
extern HistorySource historySource;
const char* historySourceName();          // short text for the Service tab
bool     statsHasData(const WaterStats& s);
bool     statsValid(const WaterStats& s);   // plausible values (used before a restore)
bool     statsRestore(const WaterStats& in); // validates, keeps the current tank age, saves

// Cycle-in-progress marker (2.9). Written when a cycle starts and at each step,
// removed when it ends. If it is still there at power-up, the cycle was cut short
// by a power cut, a crash or a watchdog restart.
void     cycleMarkStep(int state);
void     cycleMarkEnd();
bool     cycleWasInterrupted(int& state, uint32_t& epoch);
bool     timeIsValid();                  // true once NTP has set the clock
uint32_t currentMonthKey();              // YYYYMM, or 0 if the clock is not set

#endif
