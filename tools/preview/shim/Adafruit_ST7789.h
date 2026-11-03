// Host stand-in for the panel: every pushed frame goes to host_frame(), rotation is only remembered.
#pragma once
#include <Adafruit_GFX.h>

#define ST77XX_BLACK 0x0000
#define ST77XX_WHITE 0xFFFF

void host_frame(const uint16_t* buf, int w, int h);
extern int g_host_rotation;

struct Adafruit_ST7789 {
  Adafruit_ST7789(int, int, int) {}
  void setSPISpeed(uint32_t) {}
  void init(int, int) {}
  void setRotation(int r) { g_host_rotation = r; }
  uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  }
  void drawRGBBitmap(int, int, uint16_t* b, int w, int h) { host_frame(b, w, h); }
};
