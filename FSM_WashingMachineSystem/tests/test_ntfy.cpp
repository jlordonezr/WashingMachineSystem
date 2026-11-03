// Tests for Ntfy.h (phone alert topic, server address, JSON message, retries).
// g++ -std=c++17 -O1 -Wall test_ntfy.cpp -o test_ntfy && ./test_ntfy
#include "../Ntfy.h"
#include <cstdio>

static int passes = 0, failures = 0;
#define CHECK(c, ...) do { if (c) ++passes; else { ++failures; printf("  FAIL %s: ", __func__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// N1: topics ntfy accepts, and the ones it refuses.
static void N1() {
  CHECK(ntfyTopicValid("greywater-abc23_XY"), "normal topic refused");
  CHECK(!ntfyTopicValid(""), "empty topic accepted");
  CHECK(!ntfyTopicValid("my topic"), "space accepted");
  CHECK(!ntfyTopicValid("lavadora-reúso"), "accent accepted");
  CHECK(!ntfyTopicValid("a/b"), "slash accepted");
  char t64[65]; memset(t64, 'a', 64); t64[64] = '\0';
  CHECK(ntfyTopicValid(t64), "64 characters refused");
}

// N2: the server address is tidied; a pasted subscribe link loses its topic.
static void N2() {
  char s[64];
  strcpy(s, " https://ntfy.sh/ ");                 ntfyNormalizeServer(s, "t1");
  CHECK(!strcmp(s, "https://ntfy.sh"), "spaces and slash kept: '%s'", s);
  strcpy(s, "https://ntfy.sh/greywater-x");        ntfyNormalizeServer(s, "greywater-x");
  CHECK(!strcmp(s, "https://ntfy.sh"), "pasted topic kept: '%s'", s);
  strcpy(s, "https://ntfy.sh/greywater-x/");       ntfyNormalizeServer(s, "greywater-x");
  CHECK(!strcmp(s, "https://ntfy.sh"), "pasted topic with slash kept: '%s'", s);
  strcpy(s, "https://ntfy.example.com/ntfy");      ntfyNormalizeServer(s, "greywater-x");
  CHECK(!strcmp(s, "https://ntfy.example.com/ntfy"), "other path changed: '%s'", s);
  strcpy(s, "https://greywater-x");                ntfyNormalizeServer(s, "greywater-x");
  CHECK(!strcmp(s, "https://greywater-x"), "host taken for the topic: '%s'", s);
}

// N3: the JSON body carries accents, quotes and the optional fields.
static void N3() {
  char out[1300];
  CHECK(ntfyBuildJson(out, sizeof(out), "gw-t", "Reúso de Agua: tanque lleno", "Say \"hi\"\nnow", 9, "droplet", "http://192.168.1.5/"),
        "message did not fit");
  CHECK(!strcmp(out, "{\"topic\":\"gw-t\",\"title\":\"Reúso de Agua: tanque lleno\",\"message\":\"Say \\\"hi\\\"\\u000anow\","
                     "\"priority\":5,\"tags\":[\"droplet\"],\"click\":\"http://192.168.1.5/\"}"), "wrong JSON: %s", out);
  CHECK(ntfyBuildJson(out, sizeof(out), "gw-t", "T", "M", 0, "", ""), "plain message did not fit");
  CHECK(!strcmp(out, "{\"topic\":\"gw-t\",\"title\":\"T\",\"message\":\"M\",\"priority\":1}"), "wrong plain JSON: %s", out);
  CHECK(!ntfyBuildJson(out, 20, "gw-t", "T", "M", 3, "", ""), "overflow not reported");
}

// N4: a cut never splits a UTF-8 character or an escape.
static void N4() {
  char out[8];
  jsonEscape(out, 7, "abcdeú");                  // 'ú' needs 2 bytes, only 1 left before the terminator
  CHECK(!strcmp(out, "abcde"), "accent split: '%s'", out);
  jsonEscape(out, sizeof(out), "abcdeú");
  CHECK(!strcmp(out, "abcdeú"), "accent dropped although it fits: '%s'", out);
  jsonEscape(out, 6, "abcd\"e");                 // \" needs 2 bytes, only 1 left
  CHECK(!strcmp(out, "abcd"), "escape split: '%s'", out);
  jsonEscape(out, 6, "ab\ncd");
  CHECK(!strcmp(out, "ab"), "control escape split: '%s'", out);
}

// N5: only failures that may pass on a later try are tried again.
static void N5() {
  CHECK(ntfyRetryable(-1) && ntfyRetryable(-11) && ntfyRetryable(NTFY_NO_WIFI), "transport error not retried");
  CHECK(ntfyRetryable(429) && ntfyRetryable(502) && ntfyRetryable(408), "busy server not retried");
  CHECK(!ntfyRetryable(200) && !ntfyRetryable(400) && !ntfyRetryable(403) && !ntfyRetryable(NTFY_BAD_URL), "permanent failure retried");
  CHECK(NTFY_NO_WIFI < -11 && NTFY_BAD_URL < -11, "codes overlap HTTPClient's (-1..-11)");
}

// N6: an alert keeps trying through an outage, at growing intervals, then gives up (3.3.1).
static void N6() {
  CHECK(ntfyRetryDelayMs(1, 5000, 300000) == 5000 && ntfyRetryDelayMs(2, 5000, 300000) == 10000 &&
        ntfyRetryDelayMs(3, 5000, 300000) == 20000, "delays are not 5 s, 10 s, 20 s");
  CHECK(ntfyRetryDelayMs(7, 5000, 300000) == 300000 && ntfyRetryDelayMs(200, 5000, 300000) == 300000, "delay not capped at 5 min");
  CHECK(ntfyRetryDelayMs(0, 5000, 300000) == 5000, "no tries yet (no Wi-Fi): not the first delay");
  CHECK(ntfyKeepTrying(-1, 60000, 1800000) && ntfyKeepTrying(NTFY_NO_WIFI, 60000, 1800000) && ntfyKeepTrying(503, 0, 1800000),
        "outage not tried again");
  CHECK(!ntfyKeepTrying(-1, 1800000, 1800000), "still trying after the window");
  CHECK(!ntfyKeepTrying(200, 0, 1800000) && !ntfyKeepTrying(400, 0, 1800000) && !ntfyKeepTrying(NTFY_BAD_URL, 0, 1800000),
        "delivered or refused alert tried again");
  // A 30 min outage: tries at 0, 5, 15, 35, 75, 155, 315, 615, 915 ... s, never more often than every 5 min after that.
  unsigned long t = 0; int tries = 0;
  for (uint8_t a = 1; ntfyKeepTrying(-1, t, 1800000); ++a) { ++tries; t += ntfyRetryDelayMs(a, 5000, 300000); }
  CHECK(tries >= 10 && tries <= 14, "%d tries in a 30 min outage", tries);
}

int main() {
  N1(); N2(); N3(); N4(); N5(); N6();
  printf("%d checks passed, %d failed\n", passes, failures);
  return failures ? 1 : 0;
}
