// Host stand-in for the Arduino core: virtual clock, scripted serial, recorded backlight.
#pragma once
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include "Print.h"

class __FlashStringHelper;
struct String {
  std::string v;
  size_t length() const { return v.size(); }
  const char* c_str() const { return v.c_str(); }
};

#define radians(d) ((d) * 0.017453292519943295)
#define constrain(x, low, high) ((x) < (low) ? (low) : ((x) > (high) ? (high) : (x)))
#define PROGMEM
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define PSTR(x) (x)
#define F(x) (x)
#define ESP_ARDUINO_VERSION_MAJOR 3
typedef bool boolean;
using std::max;
using std::min;

inline size_t host_strlcpy(char* d, const char* s, size_t n) {
  size_t len = strlen(s);
  if (n) {
    size_t k = len < n - 1 ? len : n - 1;
    memcpy(d, s, k);
    d[k] = 0;
  }
  return len;
}
#define strlcpy host_strlcpy

extern unsigned long g_host_ms;
inline unsigned long millis() { return g_host_ms; }
inline unsigned long micros() { return g_host_ms * 1000; }
inline void delay(unsigned long ms) { g_host_ms += ms; }
inline void yield() {}

extern int g_host_blk_pin, g_host_blk_duty, g_host_blk_bits;
inline void pinMode(int, int) {}
inline void digitalWrite(int pin, int v) {
  g_host_blk_pin = pin;
  g_host_blk_duty = v ? -1 : 0;  // -1: driven fully on without LEDC
}
inline bool ledcAttach(int pin, uint32_t, uint8_t bits) {
  g_host_blk_pin = pin;
  g_host_blk_bits = bits;
  return true;
}
inline bool ledcWrite(int, uint32_t duty) {
  g_host_blk_duty = duty;
  return true;
}

// Input comes from the scene; everything printed is counted, so "silent on success" can be checked.
struct HostSerial {
  std::string rx;
  size_t rxPos = 0, txBytes = 0;
  bool echo = false;
  void setRxBufferSize(size_t) {}
  void begin(long) {}
  int available() { return rxPos < rx.size(); }
  int read() { return (uint8_t)rx[rxPos++]; }
  void tx(const char* s, size_t n) {
    txBytes += n;
    if (echo) fwrite(s, 1, n, stderr);
  }
  void print(const char* s) { tx(s, strlen(s)); }
  void println(const char* s) {
    print(s);
    tx("\n", 1);
  }
  void println() { tx("\n", 1); }
  void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    char b[256];
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, a);
    va_end(a);
    if (n > 0) tx(b, n);
  }
};
extern HostSerial Serial;
