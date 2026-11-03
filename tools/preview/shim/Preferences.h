// Host stand-in for NVS: a process-wide map that counts every write, so debounce can be measured.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <map>
#include <string>

extern std::map<std::string, uint8_t> g_host_nvs;
extern int g_host_nvs_writes;

struct Preferences {
  bool begin(const char*, bool = false) { return true; }
  void end() {}
  uint8_t getUChar(const char* key, uint8_t fallback = 0) {
    auto it = g_host_nvs.find(key);
    return it == g_host_nvs.end() ? fallback : it->second;
  }
  size_t putUChar(const char* key, uint8_t v) {
    g_host_nvs[key] = v;
    g_host_nvs_writes++;
    return 1;
  }
};
