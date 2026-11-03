// Host stand-in for the part of Arduino's Print that Adafruit GFX and the sketch use.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

class Print {
 public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t* b, size_t n) {
    size_t k = 0;
    while (n--) k += write(*b++);
    return k;
  }
  size_t print(const char* s) {
    size_t k = 0;
    while (*s) k += write((uint8_t)*s++);
    return k;
  }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(unsigned char v) { return printNumber(v); }
  size_t print(int v) { return printNumber(v); }
  size_t println(const char* s) { return print(s) + write('\n'); }

 private:
  size_t printNumber(int v) {
    char b[16];
    snprintf(b, sizeof(b), "%d", v);
    return print(b);
  }
};
