#pragma once
#include <map>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
struct Preferences {
  static std::map<std::string,std::vector<uint8_t>> store;
  bool begin(const char*, bool){return true;}
  size_t getBytesLength(const char* k){auto it=store.find(k);return it==store.end()?0:it->second.size();}
  size_t getBytes(const char* k, void* b, size_t n){auto it=store.find(k);if(it==store.end())return 0;size_t c=n<it->second.size()?n:it->second.size();memcpy(b,it->second.data(),c);return c;}
  bool remove(const char* k){return store.erase(k)>0;}
  size_t putBytes(const char* k, const void* b, size_t n){store[k]=std::vector<uint8_t>((const uint8_t*)b,(const uint8_t*)b+n);return n;}
};
