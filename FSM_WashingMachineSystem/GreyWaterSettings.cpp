#include "GreyWaterSettings.h"
#include "GreyWaterSystem.h"
#include <Preferences.h>
#include <time.h>
#include <stddef.h>

Settings   cfg;
WaterStats stats;
HistorySource historySource = historyEmpty;

static Preferences prefs;

// Reads one stored history record. Returns 1 for the current format, 2 for a
// migrated version 1 record, 0 if missing or unreadable.
static int loadStats(const char* key, WaterStats& out) {
  size_t len = prefs.getBytesLength(key);
  WaterStats s;
  memset(&s, 0, sizeof(s));
  if (len == sizeof(WaterStats) && prefs.getBytes(key, &s, sizeof(s)) == sizeof(s) &&
      s.version == STATS_VERSION) {
    out = s; return 1;
  }
  if (len == offsetof(WaterStats, stepAvgS) && prefs.getBytes(key, &s, len) == len && s.version == 2) {
    // Version 2 stored everything up to lastValid, in the same layout.
    s.version = STATS_VERSION; out = s; return 2;
  }
  if (len == offsetof(WaterStats, prevMonthKey) && prefs.getBytes(key, &s, len) == len && s.version == 1) {
    // Version 1 stored only the fields up to tankSinceEpoch, in the same layout.
    s.version = STATS_VERSION; out = s; return 2;
  }
  return 0;
}

// Copies an unreadable record to "stats_keep" once, so it is never lost.
static void keepUnreadable(const char* key) {
  if (prefs.getBytesLength("stats_keep") > 0) return;
  size_t len = prefs.getBytesLength(key);
  if (len == 0 || len > 512) return;
  uint8_t raw[512];
  if (prefs.getBytes(key, raw, len) == len) prefs.putBytes("stats_keep", raw, len);
}

void settingsDefaults(Settings& s) {
  memset(&s, 0, sizeof(s));
  s.version          = SETTINGS_VERSION;
  s.washPeriodMs     = TANK_WASH_PERIOD;
  s.washOpenMs       = TANK_WASH_OPEN_TIME;
  s.drain2DurationMs = TRANSITION_DELAY;
  s.edgeDetectMs     = EDGE_DETECTION_DELAY;
  s.stepAlertMs      = STEP_ALERT_DELAY;
  s.tankMaxAgeHours  = TANK_MAX_AGE_HOURS;
  s.manualWashMs     = MANUAL_WASH_TIME;
  s.flowOnLpm        = FLOW_ON_LPM;
  s.minFillLitres    = MIN_FILL_LITRES;
  s.buzzerOn         = 0;
  s.language         = LANG_ES;
  s.allowRemoteStart = true;   // dashboard Start button; asks for confirmation
  strlcpy(s.ntfyServer,  DEFAULT_NTFY_SERVER,  sizeof(s.ntfyServer));
  strlcpy(s.ntfyTopic,   "",                   sizeof(s.ntfyTopic));
  strlcpy(s.timeZone,    DEFAULT_TIME_ZONE,    sizeof(s.timeZone));
  strlcpy(s.webPassword, DEFAULT_WEB_PASSWORD, sizeof(s.webPassword));
  strlcpy(s.otaPassword, DEFAULT_OTA_PASSWORD, sizeof(s.otaPassword));
}

static uint32_t clampU32(uint32_t v, uint32_t lo, uint32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

void settingsSanitize(Settings& s) {
  s.washPeriodMs     = clampU32(s.washPeriodMs, 1000, 600000);           // 1 s .. 10 min
  s.washOpenMs       = clampU32(s.washOpenMs, OPERATION_DELAY, s.washPeriodMs - OPERATION_DELAY);
  s.drain2DurationMs = clampU32(s.drain2DurationMs, 60000, 7200000);     // 1 .. 120 min
  s.edgeDetectMs     = clampU32(s.edgeDetectMs, 1000, 30000);            // 1 .. 30 s
  s.stepAlertMs      = clampU32(s.stepAlertMs, 600000, 36000000);        // 10 .. 600 min
  s.tankMaxAgeHours  = (uint16_t)clampU32(s.tankMaxAgeHours, 1, 240);    // 1 h .. 10 days
  if (!(s.flowOnLpm >= 0.2f && s.flowOnLpm <= 10.0f)) s.flowOnLpm = FLOW_ON_LPM;  // also catches NaN
  if (!(s.minFillLitres >= 0.5f && s.minFillLitres <= 50.0f)) s.minFillLitres = MIN_FILL_LITRES;
  s.manualWashMs     = clampU32(s.manualWashMs, 60000, 1800000);         // 1 .. 30 min
  s.ntfyServer[sizeof(s.ntfyServer) - 1]   = '\0';
  s.ntfyTopic[sizeof(s.ntfyTopic) - 1]     = '\0';
  s.timeZone[sizeof(s.timeZone) - 1]       = '\0';
  s.webPassword[sizeof(s.webPassword) - 1] = '\0';
  s.otaPassword[sizeof(s.otaPassword) - 1] = '\0';
  if (s.timeZone[0] == '\0')    strlcpy(s.timeZone, DEFAULT_TIME_ZONE, sizeof(s.timeZone));
  if (s.webPassword[0] == '\0') strlcpy(s.webPassword, DEFAULT_WEB_PASSWORD, sizeof(s.webPassword));
  if (s.otaPassword[0] == '\0') strlcpy(s.otaPassword, DEFAULT_OTA_PASSWORD, sizeof(s.otaPassword));
  if (s.language != LANG_EN) s.language = LANG_ES;
}

void settingsBegin() {
  prefs.begin("greywater", false);

  settingsDefaults(cfg);
  Settings stored;
  if (prefs.getBytesLength("cfg") == sizeof(Settings) &&
      prefs.getBytes("cfg", &stored, sizeof(stored)) == sizeof(stored) &&
      stored.version == SETTINGS_VERSION) {
    cfg = stored;
  } else if (prefs.getBytesLength("cfg") == sizeof(Settings) &&
             prefs.getBytes("cfg", &stored, sizeof(stored)) == sizeof(stored) &&
             stored.version == 6) {
    // Version 6 has the same layout up to buzzerOn; language sat in the padding.
    stored.version  = SETTINGS_VERSION;
    stored.language = LANG_ES;
    cfg = stored;
    settingsSanitize(cfg);
    settingsSave();
  } else if (prefs.getBytesLength("cfg") == offsetof(Settings, buzzerOn) &&
             prefs.getBytes("cfg", &stored, offsetof(Settings, buzzerOn)) == offsetof(Settings, buzzerOn) &&
             stored.version == 5) {
    // Version 5 stored the same fields up to minFillLitres: keep them, add the new default.
    stored.version  = SETTINGS_VERSION;
    stored.buzzerOn = 0;
    stored.language = LANG_ES;
    cfg = stored;
    settingsSanitize(cfg);
    settingsSave();
  } else if (prefs.getBytesLength("cfg") == offsetof(Settings, minFillLitres) &&
             prefs.getBytes("cfg", &stored, offsetof(Settings, minFillLitres)) == offsetof(Settings, minFillLitres) &&
             stored.version == 4) {
    // Version 4 stored the same fields up to flowOnLpm: keep them, add the new default.
    stored.version       = SETTINGS_VERSION;
    stored.minFillLitres = MIN_FILL_LITRES;
    stored.buzzerOn      = 0;
    stored.language      = LANG_ES;
    cfg = stored;
    settingsSanitize(cfg);
    settingsSave();
  }
  settingsSanitize(cfg);

  memset(&stats, 0, sizeof(stats));
  stats.version = STATS_VERSION;

  int got = loadStats("stats", stats);
  if (got) {
    historySource = (got == 2) ? historyMigrated : historyFlash;
  } else if ((got = loadStats("stats_bak", stats)) != 0) {
    if (prefs.getBytesLength("stats") > 0) keepUnreadable("stats");
    historySource = historyBackup;          // main record unreadable: the backup copy saved the history
  } else if (prefs.getBytesLength("stats") > 0 || prefs.getBytesLength("stats_bak") > 0) {
    // A record exists but this firmware cannot read it (for example, a future
    // or damaged format). Keep the raw bytes aside instead of overwriting them,
    // so a later firmware version can still recover them.
    keepUnreadable("stats");
    historySource = historyUnreadable;
  } else {
    historySource = historyEmpty;
  }
  if (got) statsSave();                     // refresh both copies in the current format
}

bool statsRollMonth() {
  uint32_t month = currentMonthKey();
  if (month == 0 || month == stats.monthKey) return false;
  if (stats.monthKey == 0) {
    // Washes recorded before the clock was set belong to the current month.
    stats.monthKey = month;
    return true;
  }
  if (stats.monthTapL + stats.monthReusedL + stats.monthSavedL > 0.0f || stats.monthCycles > 0) {
    stats.prevMonthKey     = stats.monthKey;
    stats.prevMonthTapL    = stats.monthTapL;
    stats.prevMonthReusedL = stats.monthReusedL;
    stats.prevMonthSavedL  = stats.monthSavedL;
    stats.prevMonthCycles  = stats.monthCycles;
  }
  stats.monthKey     = month;
  stats.monthTapL    = stats.monthReusedL = stats.monthSavedL = 0.0f;
  stats.monthCycles  = 0;
  return true;
}

bool settingsSave() {
  settingsSanitize(cfg);
  return prefs.putBytes("cfg", &cfg, sizeof(cfg)) == sizeof(cfg);
}

void statsSave() {
  prefs.putBytes("stats", &stats, sizeof(stats));
  prefs.putBytes("stats_bak", &stats, sizeof(stats));
}

static bool finiteLitres(float v) { return v == v && v >= 0.0f && v < 1.0e8f; }  // v == v rejects NaN

static bool validMonthKey(uint32_t k) {
  return k == 0 || (k >= 200001 && k <= 299912 && (k % 100) >= 1 && (k % 100) <= 12);
}

bool statsHasData(const WaterStats& s) {
  return s.totalCycles > 0 || s.totalTapL + s.totalReusedL + s.totalSavedL > 0.5f || s.lastValid;
}

bool statsValid(const WaterStats& s) {
  return finiteLitres(s.totalTapL) && finiteLitres(s.totalReusedL) && finiteLitres(s.totalSavedL) &&
         finiteLitres(s.monthTapL) && finiteLitres(s.monthReusedL) && finiteLitres(s.monthSavedL) &&
         finiteLitres(s.prevMonthTapL) && finiteLitres(s.prevMonthReusedL) && finiteLitres(s.prevMonthSavedL) &&
         finiteLitres(s.lastTapL) && finiteLitres(s.lastReusedL) && finiteLitres(s.lastSavedL) &&
         s.totalCycles < 1000000 && s.monthCycles <= s.totalCycles + 1000 && s.prevMonthCycles < 100000 &&
         validMonthKey(s.monthKey) && validMonthKey(s.prevMonthKey) && s.lastDurationS < 864000 &&
         s.lastCompleted <= 1 && s.lastValid <= 1 &&
         s.stepAvgS[0] < 864000 && s.stepAvgS[1] < 864000 && s.stepAvgS[2] < 864000;
}

bool statsRestore(const WaterStats& in) {
  if (!statsValid(in)) return false;
  uint32_t tankSince = stats.tankSinceEpoch;   // the tank's state is the controller's own, not the backup's
  stats = in;
  stats.version = STATS_VERSION;
  stats.tankSinceEpoch = tankSince;
  statsSave();
  historySource = historyRestored;
  return true;
}

const char* historySourceName() {
  switch (historySource) {
    case historyFlash:      return "Loaded from the controller";
    case historyBackup:     return "Recovered from the backup copy";
    case historyMigrated:   return "Upgraded from an older firmware";
    case historyRestored:   return "Restored from the app";
    case historyUnreadable: return "Stored record unreadable (kept aside)";
    default:                return "No history stored";
  }
}

bool timeIsValid() {
  return time(nullptr) > 1700000000;  // any date after Nov 2023 means NTP has synced
}

uint32_t currentMonthKey() {
  if (!timeIsValid()) return 0;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  return (uint32_t)(t.tm_year + 1900) * 100 + (t.tm_mon + 1);
}

// ── Cycle-in-progress marker ───────────────────────────────────────────────
struct CycleMark { uint8_t state; uint32_t epoch; };

void cycleMarkStep(int state) {
  CycleMark m = { (uint8_t)state, timeIsValid() ? (uint32_t)time(nullptr) : 0 };
  prefs.putBytes("cyc", &m, sizeof(m));
}

void cycleMarkEnd() {
  if (prefs.getBytesLength("cyc") > 0) prefs.remove("cyc");
}

bool cycleWasInterrupted(int& state, uint32_t& epoch) {
  CycleMark m;
  if (prefs.getBytesLength("cyc") != sizeof(m) || prefs.getBytes("cyc", &m, sizeof(m)) != sizeof(m)) return false;
  state = m.state; epoch = m.epoch;
  return true;
}
