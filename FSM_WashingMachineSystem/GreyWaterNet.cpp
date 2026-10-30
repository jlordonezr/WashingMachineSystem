#include "GreyWaterNet.h"
#include "GreyWaterSystem.h"
#include "GreyWaterSettings.h"
#include "DashboardPage.h"

#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

static WiFiManager wm;
static WebServer   server(80);
static void blockSetupPages();        // see netStartConfigPortal()

static bool servicesStarted = false;   // mDNS, OTA, NTP and web server are running
static bool portalActive    = false;   // Wi-Fi setup page is open
static unsigned long lastRetry = 0;

// ── Phone alerts (ntfy) ────────────────────────────────────────────────────
// Sending an HTTPS request takes about a second, far too long for loop().
// notify() copies the alert into a queue and a separate FreeRTOS task on core 0
// (the Wi-Fi core) sends it, so the wash cycle never waits for the network.
// Alerts are published as JSON to the server's root URL (ntfy "publish as
// JSON", from 3.1). JSON carries UTF-8, so Spanish titles with accents arrive
// intact; HTTP headers (used up to 3.0) only carry plain ASCII.
struct NotifyItem {
  char    url[136];      // server, e.g. https://ntfy.sh
  char    topic[64];
  char    title[80];
  char    body[360];
  char    tags[32];
  uint8_t priority;
};

static void jsonEscape(char* dst, size_t size, const char* src);

// "Grey water: tank full" -> "tank full", for the event log.
static const char* shortTitle(const char* t) {
  if (strncmp(t, "Grey water: ", 12) == 0)   return t + 12;
  if (strncmp(t, "Aguas grises: ", 14) == 0) return t + 14;
  return t;
}

static QueueHandle_t notifyQueue = nullptr;

// Delivery results travel back to loop() through a second queue, because the
// event log is only ever written from loop() (it has no lock).
struct NotifyResult { char title[80]; int code; };   // code: HTTP status, or <0 = not sent
static QueueHandle_t resultQueue = nullptr;
static uint32_t alertSeq = 0;      // results received so far (the app watches it change)
static int      alertLastCode = 0;
static bool     alertLastOk = false;

static void reportResult(const NotifyItem& item, int code) {
  if (!resultQueue) return;
  NotifyResult r;
  strlcpy(r.title, item.title, sizeof(r.title));
  r.code = code;
  xQueueSend(resultQueue, &r, 0);
}

static void notifyTask(void*) {
  NotifyItem item;
  for (;;) {
    if (xQueueReceive(notifyQueue, &item, portMAX_DELAY) != pdTRUE) continue;

    // Wait up to 60 s for Wi-Fi, so an alert raised during a short drop-out is not lost.
    for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; ++i) vTaskDelay(pdMS_TO_TICKS(1000));
    if (WiFi.status() != WL_CONNECTED) { reportResult(item, -2); continue; }   // -2: no Wi-Fi for 60 s

    WiFiClientSecure secureClient;
    WiFiClient       plainClient;
    HTTPClient       http;
    bool ok;
    if (strncmp(item.url, "https://", 8) == 0) {
      secureClient.setInsecure();   // no certificate check: alerts carry no secrets
      ok = http.begin(secureClient, item.url);
    } else {
      ok = http.begin(plainClient, item.url);
    }
    if (!ok) { reportResult(item, -3); continue; }                            // -3: bad server address

    static char topic[140], title[170], body[740], tags[70], json[1200];   // this task only
    jsonEscape(topic, sizeof(topic), item.topic);
    jsonEscape(title, sizeof(title), item.title);
    jsonEscape(body,  sizeof(body),  item.body);
    jsonEscape(tags,  sizeof(tags),  item.tags);
    snprintf(json, sizeof(json), "{\"topic\":\"%s\",\"title\":\"%s\",\"message\":\"%s\",\"priority\":%u%s%s%s}",
             topic, title, body, item.priority, tags[0] ? ",\"tags\":[\"" : "", tags, tags[0] ? "\"]" : "");
    http.setTimeout(8000);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST((uint8_t*)json, strlen(json));
    DEBUG_PRINT("Alert sent, HTTP status: ");
    DEBUG_PRINT(code);
    http.end();
    reportResult(item, code);                                                  // 200 = delivered
  }
}

void notify(const char* title, const char* body, uint8_t priority, const char* tags) {
  logEvent("Alert: %s", shortTitle(title));
  if (notifyQueue == nullptr || cfg.ntfyTopic[0] == '\0') return;

  NotifyItem item;
  memset(&item, 0, sizeof(item));
  size_t len = strlcpy(item.url, cfg.ntfyServer, sizeof(item.url));
  if (len > 0 && len < sizeof(item.url) && item.url[len - 1] == '/') item.url[len - 1] = '\0';
  strlcat(item.url, "/", sizeof(item.url));                 // JSON is posted to the server root
  strlcpy(item.topic, cfg.ntfyTopic, sizeof(item.topic));
  strlcpy(item.title, title, sizeof(item.title));
  strlcpy(item.body,  body,  sizeof(item.body));
  strlcpy(item.tags,  tags ? tags : "", sizeof(item.tags));
  item.priority = (priority < 1) ? 1 : (priority > 5 ? 5 : priority);

  if (xQueueSend(notifyQueue, &item, 0) != pdTRUE) {   // never block; drop the alert if the queue is full
    logEvent("Alert not sent: queue full");
  }
}

// Called from netLoop(): logs what happened to each alert.
static void drainAlertResults() {
  NotifyResult r;
  while (resultQueue && xQueueReceive(resultQueue, &r, 0) == pdTRUE) {
    const char* t = shortTitle(r.title);
    alertSeq++; alertLastCode = r.code; alertLastOk = (r.code >= 200 && r.code < 300);
    if (alertLastOk)        logEvent("Alert delivered: %s", t);
    else if (r.code == -2)  logEvent("Alert not sent (no Wi-Fi): %s", t);
    else                    logEvent("Alert failed (code %d): %s", r.code, t);
  }
}

// ── JSON helpers ───────────────────────────────────────────────────────────
// Copies src into dst with JSON escaping, so settings text such as a time
// zone with quotes cannot break the response.
static void jsonEscape(char* dst, size_t size, const char* src) {
  size_t o = 0;
  for (; *src && o + 7 < size; ++src) {
    unsigned char ch = (unsigned char)*src;
    if (ch == '"' || ch == '\\') { dst[o++] = '\\'; dst[o++] = ch; }
    else if (ch < 0x20)          { o += snprintf(dst + o, size - o, "\\u%04x", ch); }
    else                         { dst[o++] = ch; }
  }
  dst[o] = '\0';
}

static const char* levelName(WaterLevel l) {
  return l == high ? "high" : (l == medium ? "medium" : "low");
}

static void sendJson(int code, const char* json) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", json);
}

static void sendError(int code, const char* message) {
  char esc[160], buf[200];
  jsonEscape(esc, sizeof(esc), message);
  snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", esc);
  sendJson(code, buf);
}

// ── Access control ─────────────────────────────────────────────────────────
// Every page needs the dashboard password (HTTP Basic, user "admin").
// Actions also need the X-GW header, which a form on another website cannot
// send, so a malicious page cannot press Stop or change settings using the
// password the browser remembers.
// After 5 wrong passwords within a minute from one device (IP address), that
// device is refused for a minute. Other devices are not affected, so nobody on
// the network can lock the owner out (2.9 locked everyone out).
struct AuthSlot { uint32_t ip; uint8_t fails; bool locked; unsigned long windowStart, lockStart, lastSeen; };
static AuthSlot authSlots[AUTH_SLOTS];

static AuthSlot& authSlotFor(uint32_t ip) {
  AuthSlot* oldest = &authSlots[0];
  for (AuthSlot& a : authSlots) {
    if (a.ip == ip) return a;
    if (a.lastSeen < oldest->lastSeen) oldest = &a;
  }
  memset(oldest, 0, sizeof(*oldest));   // reuse the least recently seen slot
  oldest->ip = ip;
  return *oldest;
}

static bool authorized() {
  uint32_t ip = (uint32_t)server.client().remoteIP();
  AuthSlot& a = authSlotFor(ip);
  a.lastSeen = millis();
  if (a.locked && millis() - a.lockStart < 60000UL) {
    sendError(429, "Too many wrong passwords from this device. Wait one minute and try again.");
    return false;
  }
  a.locked = false;
  if (server.authenticate(WEB_USER, cfg.webPassword)) { a.fails = 0; return true; }
  if (server.hasHeader("Authorization")) {                // a password was given, and it was wrong
    if (millis() - a.windowStart > 60000UL) { a.windowStart = millis(); a.fails = 0; }
    if (++a.fails >= 5) {
      a.locked = true; a.lockStart = millis(); a.fails = 0;
      logEvent("Device %s locked for 1 minute: 5 wrong passwords", server.client().remoteIP().toString().c_str());
      sendError(429, "Too many wrong passwords from this device. Wait one minute and try again.");
      return false;
    }
  }
  server.requestAuthentication(BASIC_AUTH, "Grey water controller");
  return false;
}

static bool actionAllowed() {
  if (!authorized()) return false;
  if (server.header("X-GW") != "1") { sendError(403, "Request blocked: use the dashboard page."); return false; }
  return true;
}

// ── Web handlers ───────────────────────────────────────────────────────────
static void handleRoot() {
  if (!authorized()) return;
  // Sent compressed (about 21 KB instead of 77 KB): every browser unpacks it.
  // Less data means less time inside handleClient(), which runs in loop().
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html; charset=utf-8", (const char*)DASHBOARD_HTML_GZ, DASHBOARD_HTML_GZ_LEN);
}

static void handleStatus() {
  if (!authorized()) return;
  SystemStatus s;
  getSystemStatus(s);
  int intrStep; uint32_t intrEpoch;
  bool intrActive = interruptedInfo(intrStep, intrEpoch);

  char timeStr[20] = "";
  if (timeIsValid()) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M", &t);
  }
  char title[48], message[80];
  jsonEscape(title,   sizeof(title),   s.title);
  jsonEscape(message, sizeof(message), s.message);

  char buf[2700];
  snprintf(buf, sizeof(buf),
    "{\"fw\":\"%s\",\"state\":%d,\"stateName\":\"%s\",\"title\":\"%s\",\"message\":\"%s\","
    "\"tank\":\"%s\",\"wash\":%s,\"tapLpm\":%.2f,\"drainLpm\":%.2f,"
    "\"stepElapsedS\":%lu,\"drain2LeftS\":%lu,\"stuck\":%s,\"tankAgeH\":%.2f,\"tankMaxAgeH\":%u,"
    "\"allowRemoteStart\":%s,\"alerts\":%s,"
    "\"manualWash\":%s,\"manualWashLeftS\":%lu,\"manualWashMaxMin\":%lu,\"manualWashResult\":\"%s\","
    "\"cycle\":{\"tap\":%.2f,\"reused\":%.2f,\"saved\":%.2f},"
    "\"month\":{\"key\":%lu,\"tap\":%.2f,\"reused\":%.2f,\"saved\":%.2f,\"cycles\":%lu},"
    "\"total\":{\"tap\":%.2f,\"reused\":%.2f,\"saved\":%.2f,\"cycles\":%lu},"
    "\"prevMonth\":{\"key\":%lu,\"tap\":%.2f,\"reused\":%.2f,\"saved\":%.2f,\"cycles\":%lu},"
    "\"last\":{\"valid\":%s,\"agoS\":%ld,\"durationS\":%lu,\"tap\":%.2f,\"reused\":%.2f,\"saved\":%.2f,\"completed\":%s},"
    "\"history\":{\"source\":\"%s\",\"hasData\":%s},"
    "\"fill\":{\"litres\":%.1f,\"complete\":%s,\"minLitres\":%.1f},"
    "\"sensors\":{\"tapPulses\":%lu,\"drainPulses\":%lu,\"tapFlowing\":%s,\"drainFlowing\":%s,\"flowOnLpm\":%.1f},"
    "\"wifi\":{\"rssi\":%d,\"ip\":\"%s\"},\"time\":\"%s\",\"uptimeS\":%lu,"
    "\"outputs\":%u,\"typicalS\":[%lu,%lu,%lu],\"interrupted\":{\"active\":%s,\"step\":%d,\"epoch\":%lu},"
    "\"boot\":\"%s\",\"updatesAllowed\":%s,\"defaultPasswords\":%s,"
    "\"lang\":\"%s\",\"tankFault\":%s,\"screen\":%s,\"loop\":{\"maxMs\":%lu,\"lastMinMaxMs\":%lu},"
    "\"heap\":{\"free\":%lu,\"min\":%lu},\"alert\":{\"seq\":%lu,\"ok\":%s,\"code\":%d}}",
    FIRMWARE_VERSION, s.state, s.stateName, title, message,
    levelName(s.tankLevel), s.washValveOpen ? "true" : "false", s.tapLpm, s.drainLpm,
    (unsigned long)s.stepElapsedS, (unsigned long)s.drain2LeftS, s.stuck ? "true" : "false",
    s.tankAgeH, (unsigned)cfg.tankMaxAgeHours,
    cfg.allowRemoteStart ? "true" : "false", cfg.ntfyTopic[0] ? "true" : "false",
    s.manualWash ? "true" : "false", (unsigned long)s.manualWashLeftS,
    (unsigned long)(cfg.manualWashMs / 60000UL), s.manualWashResult,
    s.cycleTapL, s.cycleReusedL, s.cycleSavedL,
    (unsigned long)stats.monthKey, stats.monthTapL, stats.monthReusedL, stats.monthSavedL,
    (unsigned long)stats.monthCycles,
    stats.totalTapL, stats.totalReusedL, stats.totalSavedL, (unsigned long)stats.totalCycles,
    (unsigned long)stats.prevMonthKey, stats.prevMonthTapL, stats.prevMonthReusedL, stats.prevMonthSavedL,
    (unsigned long)stats.prevMonthCycles,
    stats.lastValid ? "true" : "false", (long)s.lastAgoS, (unsigned long)stats.lastDurationS,
    stats.lastTapL, stats.lastReusedL, stats.lastSavedL, stats.lastCompleted ? "true" : "false",
    historySourceName(), statsHasData(stats) ? "true" : "false",
    s.stepFillL, s.fillComplete ? "true" : "false", cfg.minFillLitres,
    (unsigned long)s.tapPulses, (unsigned long)s.drainPulses, s.tapFlowing ? "true" : "false",
    s.drainFlowing ? "true" : "false", cfg.flowOnLpm,
    (int)WiFi.RSSI(), WiFi.localIP().toString().c_str(), timeStr, (unsigned long)(millis() / 1000UL),
    (unsigned)s.outputs,
    (unsigned long)(stats.stepSamples[0] ? stats.stepAvgS[0] : 0), (unsigned long)(stats.stepSamples[1] ? stats.stepAvgS[1] : 0),
    (unsigned long)(stats.stepSamples[2] ? stats.stepAvgS[2] : 0),
    intrActive ? "true" : "false", intrStep >= stFill1 ? intrStep - 1 : 0, (unsigned long)intrEpoch,
    resetReasonTextLocal(), systemBusy() ? "false" : "true",
    (strcmp(cfg.webPassword, DEFAULT_WEB_PASSWORD) == 0 || strcmp(cfg.otaPassword, DEFAULT_OTA_PASSWORD) == 0) ? "true" : "false",
    cfg.language == LANG_EN ? "en" : "es", s.tankFault ? "true" : "false", displayAvailable() ? "true" : "false",
    (unsigned long)loopMaxMs(), (unsigned long)loopMaxLastMinuteMs(),
    (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
    (unsigned long)alertSeq, alertLastOk ? "true" : "false", alertLastCode);
  sendJson(200, buf);
}

static void buildSettingsJson(char* buf, size_t size) {
  char server_[140], topic[140], tz[100];
  jsonEscape(server_, sizeof(server_), cfg.ntfyServer);
  jsonEscape(topic,   sizeof(topic),   cfg.ntfyTopic);
  jsonEscape(tz,      sizeof(tz),      cfg.timeZone);
  snprintf(buf, size,
    "{\"language\":\"%s\",\"buzzerOn\":%s,\"minFillLitres\":%.1f,\"flowOnLpm\":%.1f,\"washPeriodS\":%.2f,\"washOpenS\":%.2f,\"drain2Min\":%lu,\"edgeS\":%.1f,"
    "\"stepAlertMin\":%lu,\"tankMaxAgeH\":%u,\"manualWashMaxMin\":%lu,\"allowRemoteStart\":%s,"
    "\"ntfyServer\":\"%s\",\"ntfyTopic\":\"%s\",\"timeZone\":\"%s\"}",
    cfg.language == LANG_EN ? "en" : "es", cfg.buzzerOn ? "true" : "false", cfg.minFillLitres, cfg.flowOnLpm, cfg.washPeriodMs / 1000.0f, cfg.washOpenMs / 1000.0f,
    (unsigned long)(cfg.drain2DurationMs / 60000UL), cfg.edgeDetectMs / 1000.0f,
    (unsigned long)(cfg.stepAlertMs / 60000UL), (unsigned)cfg.tankMaxAgeHours,
    (unsigned long)(cfg.manualWashMs / 60000UL), cfg.allowRemoteStart ? "true" : "false", server_, topic, tz);
}

static void sendSettings() {
  char buf[760];
  buildSettingsJson(buf, sizeof(buf));
  sendJson(200, buf);
}

static void handleSettingsGet() {
  if (!authorized()) return;
  sendSettings();
}

// Reads a number in user units and returns it in ms. Values out of any sane
// range are clamped BEFORE the conversion (a float too large for uint32_t is
// undefined behaviour); settingsSanitize() then applies the real limits.
static uint32_t argMs(const char* name, float scale, uint32_t current) {
  if (!server.hasArg(name) || server.arg(name).length() == 0) return current;
  double v = server.arg(name).toDouble() * scale;
  if (!(v > 0)) return current;                       // also rejects NaN
  if (v > 4.0e9) v = 4.0e9;
  return (uint32_t)(v + 0.5);
}

static void handleSettingsPost() {
  if (!actionAllowed()) return;

  Settings next = cfg;
  next.washPeriodMs     = argMs("washPeriodS",  1000.0f,  cfg.washPeriodMs);
  next.washOpenMs       = argMs("washOpenS",    1000.0f,  cfg.washOpenMs);
  next.drain2DurationMs = argMs("drain2Min",    60000.0f, cfg.drain2DurationMs);
  next.edgeDetectMs     = argMs("edgeS",        1000.0f,  cfg.edgeDetectMs);
  next.stepAlertMs      = argMs("stepAlertMin", 60000.0f, cfg.stepAlertMs);
  next.manualWashMs     = argMs("manualWashMaxMin", 60000.0f, cfg.manualWashMs);
  if (server.hasArg("tankMaxAgeH") && server.arg("tankMaxAgeH").toInt() > 0) {
    long h = server.arg("tankMaxAgeH").toInt();
    next.tankMaxAgeHours = (uint16_t)(h > 65535 ? 65535 : h);
  }
  if (server.hasArg("minFillLitres") && server.arg("minFillLitres").toFloat() > 0)
    next.minFillLitres = server.arg("minFillLitres").toFloat();
  if (server.hasArg("flowOnLpm") && server.arg("flowOnLpm").toFloat() > 0)
    next.flowOnLpm = server.arg("flowOnLpm").toFloat();
  if (server.hasArg("language")) next.language = (server.arg("language") == "en") ? LANG_EN : LANG_ES;
  if (server.hasArg("buzzerOn")) next.buzzerOn = (server.arg("buzzerOn") == "1") ? 1 : 0;
  if (server.hasArg("allowRemoteStart")) next.allowRemoteStart = (server.arg("allowRemoteStart") == "1");

  if (next.washOpenMs >= next.washPeriodMs) {
    sendError(400, "Keep the valve open for less time than the wash interval.");
    return;
  }

  if (server.hasArg("ntfyServer")) strlcpy(next.ntfyServer, server.arg("ntfyServer").c_str(), sizeof(next.ntfyServer));
  if (server.hasArg("ntfyTopic"))  strlcpy(next.ntfyTopic,  server.arg("ntfyTopic").c_str(),  sizeof(next.ntfyTopic));
  if (server.hasArg("timeZone"))   strlcpy(next.timeZone,   server.arg("timeZone").c_str(),   sizeof(next.timeZone));
  if (next.ntfyServer[0] && strncmp(next.ntfyServer, "http://", 7) != 0 && strncmp(next.ntfyServer, "https://", 8) != 0) {
    sendError(400, "The alert server must start with https:// or http://.");
    return;
  }

  const char* pwFields[2] = { "webPassword", "otaPassword" };
  char*       pwTargets[2] = { next.webPassword, next.otaPassword };
  for (int i = 0; i < 2; ++i) {
    String pw = server.hasArg(pwFields[i]) ? server.arg(pwFields[i]) : String();
    if (pw.length() == 0) continue;               // empty = keep the current password
    if (pw.length() < 8 || pw.length() > 32) { sendError(400, "Passwords need 8 to 32 characters."); return; }
    if (pw == DEFAULT_WEB_PASSWORD) { sendError(400, "Choose a password other than the default one."); return; }
    strlcpy(pwTargets[i], pw.c_str(), 33);
  }

  bool tzChanged  = strcmp(next.timeZone, cfg.timeZone) != 0;
  bool otaChanged = strcmp(next.otaPassword, cfg.otaPassword) != 0;
  bool webChanged = strcmp(next.webPassword, cfg.webPassword) != 0;
  cfg = next;
  if (!settingsSave()) { sendError(500, "Could not save the settings to flash."); return; }
  if (tzChanged && servicesStarted) configTzTime(cfg.timeZone, "pool.ntp.org", "time.google.com");
  if (otaChanged && servicesStarted) {            // 2.9 kept the old update password until a restart
    ArduinoOTA.setPassword(cfg.otaPassword);
    logEvent("Update password changed");
  }
  if (webChanged) logEvent("App password changed");
  sendSettings();
}

// ── Water history: export and restore ──────────────────────────────────────
static void handleHistoryGet() {
  if (!authorized()) return;
  char buf[1800];
  int n = snprintf(buf, sizeof(buf),
    "{\"format\":\"greywater-history\",\"version\":%u,\"fw\":\"%s\","
    "\"totalTap\":%.2f,\"totalReused\":%.2f,\"totalSaved\":%.2f,\"totalCycles\":%lu,"
    "\"monthKey\":%lu,\"monthTap\":%.2f,\"monthReused\":%.2f,\"monthSaved\":%.2f,\"monthCycles\":%lu,"
    "\"prevMonthKey\":%lu,\"prevMonthTap\":%.2f,\"prevMonthReused\":%.2f,\"prevMonthSaved\":%.2f,\"prevMonthCycles\":%lu,"
    "\"lastEndEpoch\":%lu,\"lastDurationS\":%lu,\"lastTap\":%.2f,\"lastReused\":%.2f,\"lastSaved\":%.2f,"
    "\"lastCompleted\":%u,\"lastValid\":%u,\"stepAvgS\":[%lu,%lu,%lu],\"stepSamples\":[%u,%u,%u],\"settings\":",
    (unsigned)STATS_VERSION, FIRMWARE_VERSION,
    stats.totalTapL, stats.totalReusedL, stats.totalSavedL, (unsigned long)stats.totalCycles,
    (unsigned long)stats.monthKey, stats.monthTapL, stats.monthReusedL, stats.monthSavedL, (unsigned long)stats.monthCycles,
    (unsigned long)stats.prevMonthKey, stats.prevMonthTapL, stats.prevMonthReusedL, stats.prevMonthSavedL,
    (unsigned long)stats.prevMonthCycles,
    (unsigned long)stats.lastEndEpoch, (unsigned long)stats.lastDurationS, stats.lastTapL, stats.lastReusedL,
    stats.lastSavedL, (unsigned)stats.lastCompleted, (unsigned)stats.lastValid,
    (unsigned long)stats.stepAvgS[0], (unsigned long)stats.stepAvgS[1], (unsigned long)stats.stepAvgS[2],
    (unsigned)stats.stepSamples[0], (unsigned)stats.stepSamples[1], (unsigned)stats.stepSamples[2]);
  if (n > 0 && n < (int)sizeof(buf) - 800) {
    buildSettingsJson(buf + n, sizeof(buf) - n - 2);   // settings travel with the history (no passwords)
    strlcat(buf, "}", sizeof(buf));
  }
  sendJson(200, buf);
}

static float    argF(const char* n) { return server.hasArg(n) ? server.arg(n).toFloat() : 0.0f; }
static uint32_t argU(const char* n) { return server.hasArg(n) ? (uint32_t)strtoul(server.arg(n).c_str(), nullptr, 10) : 0; }

// Restores a history the app saved (from the phone's copy or a downloaded file).
// Refused if the controller already has history, unless force=1 (the app asks
// the user to confirm first).
static void handleHistoryRestore() {
  if (!actionAllowed()) return;
  if (statsHasData(stats) && server.arg("force") != "1") {
    sendError(409, "The controller already has water history. Confirm to replace it.");
    return;
  }
  WaterStats in;
  memset(&in, 0, sizeof(in));
  in.version = STATS_VERSION;
  in.totalTapL = argF("totalTap");  in.totalReusedL = argF("totalReused");  in.totalSavedL = argF("totalSaved");
  in.totalCycles = argU("totalCycles");
  in.monthKey = argU("monthKey");   in.monthTapL = argF("monthTap");  in.monthReusedL = argF("monthReused");
  in.monthSavedL = argF("monthSaved");  in.monthCycles = argU("monthCycles");
  in.prevMonthKey = argU("prevMonthKey");  in.prevMonthTapL = argF("prevMonthTap");
  in.prevMonthReusedL = argF("prevMonthReused");  in.prevMonthSavedL = argF("prevMonthSaved");
  in.prevMonthCycles = argU("prevMonthCycles");
  in.lastEndEpoch = argU("lastEndEpoch");  in.lastDurationS = argU("lastDurationS");
  in.lastTapL = argF("lastTap");  in.lastReusedL = argF("lastReused");  in.lastSavedL = argF("lastSaved");
  in.lastCompleted = argU("lastCompleted") ? 1 : 0;  in.lastValid = argU("lastValid") ? 1 : 0;
  in.stepAvgS[0] = argU("stepAvg0");  in.stepAvgS[1] = argU("stepAvg1");  in.stepAvgS[2] = argU("stepAvg2");
  in.stepSamples[0] = argU("stepN0"); in.stepSamples[1] = argU("stepN1"); in.stepSamples[2] = argU("stepN2");
  if (!statsHasData(in))  { sendError(400, "The backup has no water history in it."); return; }
  if (!statsRestore(in))  { sendError(400, "The backup is not valid water history."); return; }
  statsRollMonth();       // if the backup is from an earlier month, start the current one
  statsSave();
  logEvent("Water history restored from the app");
  sendJson(200, "{\"ok\":true}");
}

static void handleEvents() {
  if (!authorized()) return;
  String out; out.reserve(5200); out = "[";
  for (int i = 0; i < eventCount(); ++i) {
    uint32_t ep, up; const char* text;
    if (!eventAt(i, ep, up, text)) break;
    char esc[120], line[180];
    jsonEscape(esc, sizeof(esc), text);
    snprintf(line, sizeof(line), "%s{\"epoch\":%lu,\"uptimeS\":%lu,\"text\":\"%s\"}", i ? "," : "",
             (unsigned long)ep, (unsigned long)up, esc);
    out += line;
  }
  out += "]";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

static void handleDismissInterrupted() {
  if (!actionAllowed()) return;
  dismissInterrupted();
  sendJson(200, "{\"ok\":true}");
}

static void handleStop() {
  if (!actionAllowed()) return;
  if (activeState == stIdle) { sendError(409, "No cycle is running."); return; }
  requestRemoteStop();
  sendJson(200, "{\"ok\":true}");
}

static void handleStart() {
  if (!actionAllowed()) return;
  if (!cfg.allowRemoteStart) { sendError(403, "Starting from the dashboard is turned off in Settings."); return; }
  if (!requestRemoteStart()) { sendError(409, "A cycle is already running."); return; }
  sendJson(200, "{\"ok\":true}");
}

static void handleWashStart() {
  if (!actionAllowed()) return;
  switch (startManualWash()) {
    case washBusy:     sendError(409, "Stop the cycle before washing the tank."); return;
    case washTankFull: sendError(409, "The tank is already full."); return;
    default: break;
  }
  sendJson(200, "{\"ok\":true}");
}

static void handleWashStop() {
  if (!actionAllowed()) return;
  stopManualWash();
  sendJson(200, "{\"ok\":true}");
}

static void handleTestAlert() {
  if (!actionAllowed()) return;
  if (cfg.ntfyTopic[0] == '\0') { sendError(400, "Add an alert topic in Settings first."); return; }
  notify(TR("Grey water: test alert", "Aguas grises: alerta de prueba"),
         TR("Alerts from your grey water controller are working.",
            "Las alertas de su controlador de aguas grises funcionan."), 3, "bell");
  sendJson(200, "{\"ok\":true}");
}

static void handleRestart() {
  if (!actionAllowed()) return;
  if (activeState != stIdle) { sendError(409, "Stop the cycle before restarting."); return; }
  sendJson(200, "{\"ok\":true}");
  delay(300);          // let the response reach the browser
  outputsAllOff();
  ESP.restart();
}

static void handleNotFound() {
  if (!authorized()) return;
  sendError(404, "Not found.");
}

// ── Services that need an IP address ───────────────────────────────────────
static void startServices() {
  MDNS.begin(NET_HOSTNAME);
  MDNS.addService("http", "tcp", 80);

  ArduinoOTA.setHostname(NET_HOSTNAME);
  ArduinoOTA.setPassword(cfg.otaPassword);
  ArduinoOTA.onStart([]() {
    disableLoopWDT();  // the upload blocks loop() for tens of seconds
    logEvent("Wireless firmware update started");
    outputsAllOff();   // never leave a valve open while the firmware is replaced
    statsSave();       // write the latest water history before the update
    displayShowNotice(TRS("UPDATING", "ACTUALIZANDO"), TRS("Keep power on", "No desconecte"), 600000UL);
    displayUpdate(true);
  });
  ArduinoOTA.onEnd([]() {
    displayShowNotice(TRS("UPDATED", "ACTUALIZADO"), TRS("Restarting", "Reiniciando"), 5000);
    displayUpdate(true);
  });
  ArduinoOTA.onError([](ota_error_t) {
    enableLoopWDT();
    logEvent("Wireless firmware update failed");
    displayShowNotice(TRS("UPDATE FAILED", "NO ACTUALIZADO"), TRS("Try again", "Reintente"), NOTICE_DURATION);
    displayUpdate(true);
  });
  ArduinoOTA.begin();

  configTzTime(cfg.timeZone, "pool.ntp.org", "time.google.com");

  server.begin();
  servicesStarted = true;

  char ip[20];
  strlcpy(ip, WiFi.localIP().toString().c_str(), sizeof(ip));
  displayShowNotice(ip, TRS("Connected", "Conectado"), 6000);          // the address of the dashboard
  logEvent("Wi-Fi connected: %s", ip);
  DEBUG_PRINT("Dashboard: http://greywater.local");
}

// ── Public API ─────────────────────────────────────────────────────────────
void netBegin() {
  notifyQueue = xQueueCreate(6, sizeof(NotifyItem));
  resultQueue = xQueueCreate(6, sizeof(NotifyResult));
  xTaskCreatePinnedToCore(notifyTask, "notify", 12288, nullptr, 1, nullptr, 0);

  static const char* headerKeys[] = { "X-GW" };
  server.collectHeaders(headerKeys, 1);
  server.on("/",                HTTP_GET,  handleRoot);
  server.on("/api/status",      HTTP_GET,  handleStatus);
  server.on("/api/settings",    HTTP_GET,  handleSettingsGet);
  server.on("/api/settings",    HTTP_POST, handleSettingsPost);
  server.on("/api/stop",        HTTP_POST, handleStop);
  server.on("/api/history",     HTTP_GET,  handleHistoryGet);
  server.on("/api/events",      HTTP_GET,  handleEvents);
  server.on("/api/interrupted/dismiss", HTTP_POST, handleDismissInterrupted);
  server.on("/api/history/restore", HTTP_POST, handleHistoryRestore);
  server.on("/api/start",       HTTP_POST, handleStart);
  server.on("/api/wash",        HTTP_POST, handleWashStart);
  server.on("/api/wash/stop",   HTTP_POST, handleWashStop);
  server.on("/api/test-alert",  HTTP_POST, handleTestAlert);
  server.on("/api/restart",     HTTP_POST, handleRestart);
  server.onNotFound(handleNotFound);

  WiFi.setHostname(NET_HOSTNAME);   // must come before WiFi.mode() on the ESP32 core
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  wm.setDebugOutput(DEBUG_MODE);
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT_S);
  wm.setHostname(NET_HOSTNAME);
  wm.setTitle("Grey water controller");
  static const char* menu[] = { "wifi", "info", "exit" };   // no "update" or "erase" on the setup page
  wm.setMenu(menu, 3);
  wm.setShowInfoUpdate(false);
  wm.setShowInfoErase(false);
  wm.setPreOtaUpdateCallback([]() { outputsAllOff(); });     // belt and braces: valves off if an update ever starts
  wm.setWebServerCallback(blockSetupPages);                   // no firmware upload or erase on the setup page

  if (wm.getWiFiIsSaved()) {
    WiFi.begin();              // connect in the background with the saved network
  } else {
    netStartConfigPortal();    // first boot: open the setup page
  }
}

// The setup page must not accept firmware or erase anything: it has no
// password of its own, only the setup network's. WiFiManager registers these
// pages whatever its menu says. It calls this function after creating its web
// server and BEFORE adding its own pages, and the web server uses the first
// handler that matches, so these answers win. A plain handler has no upload
// function, so an uploaded file is never passed to the updater.
// (Works with every ESP32 core; WebServer::removeRoute() exists only in 3.x.)
static void blockSetupPages() {
  if (!wm.server) return;
  auto refuse = []() { wm.server->send(404, "text/plain", "Not available on the setup page."); };
  wm.server->on("/update", HTTP_ANY, refuse);
  wm.server->on("/u",      HTTP_ANY, refuse);
  wm.server->on("/erase",  HTTP_ANY, refuse);
}

// Password of the setup network, unique to this board: "gw-" + the last six
// hex digits of its MAC address, shown on the screen while the setup page is
// open. 2.9 used the published password "greywater" for every board. Without a
// working screen the published password is used, so setup is still possible.
static const char* setupPassword() {
  static char pw[12] = "";
  if (!displayAvailable()) return WIFI_AP_PASSWORD;
  if (!pw[0]) {
    uint64_t mac = ESP.getEfuseMac();
    snprintf(pw, sizeof(pw), "gw-%02x%02x%02x", (unsigned)((mac >> 24) & 0xFF),
             (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));
  }
  return pw;
}

void netStartConfigPortal() {
  if (portalActive) return;
  if (servicesStarted) server.stop();   // the setup page needs port 80
  const char* pw = setupPassword();
  wm.startConfigPortal(WIFI_AP_NAME, pw);   // blockSetupPages() runs inside, before WiFiManager's own pages
  portalActive = true;
  char msg[24];
  snprintf(msg, sizeof(msg), TRS("Key %s", "%s"), pw);
  displayShowNotice(WIFI_AP_NAME, msg, (unsigned long)CONFIG_PORTAL_TIMEOUT_S * 1000UL);
  logEvent("Wi-Fi setup page opened");
  Serial.printf("Wi-Fi setup: join \"%s\", password %s\n", WIFI_AP_NAME, pw);
}

bool netPortalActive() { return portalActive; }
bool netIsConnected()  { return WiFi.status() == WL_CONNECTED; }

void netLoop() {
  drainAlertResults();   // log alert delivery results (also while offline)
  if (portalActive) {
    if (wm.process()) wm.stopConfigPortal();   // new network saved and connected
    if (!wm.getConfigPortalActive()) {
      portalActive = false;
      displayClearNotice();
      if (servicesStarted) server.begin();
    }
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastRetry >= WIFI_RETRY_MS) {
      lastRetry = millis();
      if (wm.getWiFiIsSaved()) WiFi.reconnect();
    }
    return;
  }

  if (!servicesStarted) startServices();
  // Wireless updates are accepted only while nothing is running: an update in
  // the middle of a wash would switch every valve off and lose the cycle.
  if (!systemBusy()) ArduinoOTA.handle();
  server.handleClient();
}
