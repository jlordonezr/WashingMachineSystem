// In-memory flash store; tests can save it to a file and load it after a simulated power cut.
#pragma once
#include <map>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdio>
struct Preferences {
  static std::map<std::string, std::vector<uint8_t>>& store() { static std::map<std::string, std::vector<uint8_t>> s; return s; }
  bool begin(const char*, bool) { return true; }
  size_t getBytesLength(const char* k) { auto it = store().find(k); return it == store().end() ? 0 : it->second.size(); }
  size_t getBytes(const char* k, void* b, size_t n) { auto it = store().find(k); if (it == store().end()) return 0;
    size_t c = n < it->second.size() ? n : it->second.size(); memcpy(b, it->second.data(), c); return c; }
  size_t putBytes(const char* k, const void* b, size_t n) { store()[k] = std::vector<uint8_t>((const uint8_t*)b, (const uint8_t*)b + n); return n; }
  bool remove(const char* k) { return store().erase(k) > 0; }
  static void saveFile(const char* path) { FILE* f = fopen(path, "wb"); for (auto& kv : store()) { uint32_t a = kv.first.size(), b = kv.second.size();
    fwrite(&a, 4, 1, f); fwrite(kv.first.data(), 1, a, f); fwrite(&b, 4, 1, f); fwrite(kv.second.data(), 1, b, f); } fclose(f); }
  static void loadFile(const char* path) { FILE* f = fopen(path, "rb"); if (!f) return; uint32_t a, b;
    while (fread(&a, 4, 1, f) == 1) { std::string k(a, ' '); fread(&k[0], 1, a, f); fread(&b, 4, 1, f);
      std::vector<uint8_t> v(b); fread(v.data(), 1, b, f); store()[k] = v; } fclose(f); }
};
