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

int main() {
  N1(); N2(); N3(); N4(); N5();
  printf("%d checks passed, %d failed\n", passes, failures);
  return failures ? 1 : 0;
}
