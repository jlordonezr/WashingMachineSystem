#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstddef>
#include <ctime>

// strlcpy/strlcat: present on the ESP32 and macOS, missing from glibc before 2.38.
#if defined(__GLIBC__) && !((__GLIBC__ > 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 38))
inline size_t strlcpy(char* d, const char* s, size_t n) { size_t l = strlen(s); if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; } return l; }
inline size_t strlcat(char* d, const char* s, size_t n) { size_t l = strnlen(d, n); return l == n ? n + strlen(s) : l + strlcpy(d + l, s, n - l); }
#endif
