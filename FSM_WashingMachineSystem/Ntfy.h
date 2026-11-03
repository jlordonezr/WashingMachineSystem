#ifndef NTFY_H
#define NTFY_H

// Phone alerts through ntfy (https://ntfy.sh): the parts that do not need the
// network, kept here (plain C, header only) so tests/test_ntfy.cpp checks them
// on the PC. GreyWaterNet.cpp sends the result.
//
// ntfy rules followed here (https://docs.ntfy.sh/publish/):
//  - A topic is 1 to 64 characters from A-Z a-z 0-9 _ - . Anything else is
//    refused by the server (HTTP 400/404), so the controller refuses it first.
//  - "Publish as JSON": POST to the server root (https://ntfy.sh/) with the
//    topic inside the JSON body. Posted anywhere else, ntfy shows the raw JSON
//    as the message text.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Result codes of an alert that never got an HTTP answer. HTTPClient uses
// -1 .. -11 for its own errors, so these stay clear of them (3.2.0 used -2 and
// -3, which HTTPClient also uses for "send failed", and the app then said
// "no Wi-Fi" or "bad server address" for a lost connection).
#define NTFY_NO_WIFI  (-100)   // Wi-Fi did not come back within the retry window
#define NTFY_BAD_URL  (-101)   // the server address could not be used

static inline bool ntfyTopicValid(const char* t) {
  size_t n = 0;
  for (; t[n]; ++n) {
    char c = t[n];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok || n >= 64) return false;
  }
  return n > 0;
}

// Tidies the server address typed in Settings, in place: removes spaces around
// it, slashes at the end and, when someone pasted the subscribe link
// (https://ntfy.sh/my-topic), the topic at the end.
static inline void ntfyNormalizeServer(char* s, const char* topic) {
  size_t start = 0, n = strlen(s);
  while (s[start] == ' ' || s[start] == '\t') ++start;
  if (start) { memmove(s, s + start, n - start + 1); n -= start; }
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '/')) s[--n] = '\0';
  size_t t = strlen(topic);
  const char* host = strstr(s, "://");
  if (t && host && n > t + 1 && s[n - t - 1] == '/' && strcmp(s + n - t, topic) == 0 &&
      (size_t)(host + 3 - s) < n - t - 1) {
    n -= t + 1;
    s[n] = '\0';
    while (n && s[n - 1] == '/') s[--n] = '\0';
  }
}

// Copies src into dst with JSON escaping. When dst is too small the text is
// cut at a whole character: half of a UTF-8 "ú" would arrive as garbage.
static inline void jsonEscape(char* dst, size_t size, const char* src) {
  size_t o = 0;
  while (*src) {
    unsigned char ch = (unsigned char)*src;
    size_t in = 1, outLen = 1;                       // bytes read from src, written to dst
    if (ch == '"' || ch == '\\') outLen = 2;
    else if (ch < 0x20)          outLen = 6;          // \u00XX
    else if (ch >= 0xC0) {                            // UTF-8 lead byte: keep the character whole
      size_t want = ch >= 0xF0 ? 4 : (ch >= 0xE0 ? 3 : 2);
      while (in < want && ((unsigned char)src[in] & 0xC0) == 0x80) ++in;
      outLen = in;
    }
    if (o + outLen + 1 > size) break;
    if (ch == '"' || ch == '\\') { dst[o++] = '\\'; dst[o++] = ch; }
    else if (ch < 0x20)          { o += snprintf(dst + o, size - o, "\\u%04x", ch); }
    else                         { memcpy(dst + o, src, in); o += in; }
    src += in;
  }
  if (size) dst[o] = '\0';
}

// Builds the "publish as JSON" body. click: page the phone opens when the
// alert is tapped (empty = none). Returns false if out was too small.
static inline bool ntfyBuildJson(char* out, size_t size, const char* topic, const char* title,
                                 const char* message, uint8_t priority, const char* tags, const char* click) {
  static char eTopic[140], eTitle[170], eMsg[740], eTags[70], eClick[100];   // one caller: the alert task
  jsonEscape(eTopic, sizeof(eTopic), topic);
  jsonEscape(eTitle, sizeof(eTitle), title);
  jsonEscape(eMsg,   sizeof(eMsg),   message);
  jsonEscape(eTags,  sizeof(eTags),  tags ? tags : "");
  jsonEscape(eClick, sizeof(eClick), click ? click : "");
  if (priority < 1) priority = 1;
  if (priority > 5) priority = 5;
  int n = snprintf(out, size, "{\"topic\":\"%s\",\"title\":\"%s\",\"message\":\"%s\",\"priority\":%u%s%s%s%s%s%s}",
                   eTopic, eTitle, eMsg, (unsigned)priority,
                   eTags[0] ? ",\"tags\":[\"" : "", eTags, eTags[0] ? "\"]" : "",
                   eClick[0] ? ",\"click\":\"" : "", eClick, eClick[0] ? "\"" : "");
  return n > 0 && (size_t)n < size;
}

// Worth another try: no answer at all, a timeout, "too many requests" or a
// server error. A 4xx (bad topic, access denied) will not fix itself.
static inline bool ntfyRetryable(int code) {
  return (code < 0 && code != NTFY_BAD_URL) || code == 408 || code == 429 || code >= 500;
}

// Wait before try number attempts+1: base, 2 x base, 4 x base ... at most maxMs.
static inline unsigned long ntfyRetryDelayMs(uint8_t attempts, unsigned long baseMs, unsigned long maxMs) {
  unsigned long d = baseMs;
  for (uint8_t i = 1; i < attempts && d < maxMs; ++i) d *= 2;
  return d < maxMs ? d : maxMs;
}

// After a failed try: keep the alert for another one? No Wi-Fi counts as
// "worth another try"; delivered or a permanent failure ends it, and so does
// the end of the retry window.
static inline bool ntfyKeepTrying(int code, unsigned long elapsedMs, unsigned long windowMs) {
  if (code >= 200 && code < 300) return false;
  return ntfyRetryable(code) && elapsedMs < windowMs;
}

#endif
