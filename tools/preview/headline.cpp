// Draws one line of text with the firmware's own label font and writes a 1-bit bitmap to stdout:
// "W H\n" then W*H bytes of 0 or 1. headline.py turns it into page/assets/headline.png.
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const int W = 320, H = 20, BASELINE = 14;
  GFXcanvas1 canvas(W, H);
  U8G2_FOR_ADAFRUIT_GFX font;
  font.begin(canvas);
  font.setFont(u8g2_font_wqy12_t_gb2312);  // same call as setup() in clawd_mood.ino
  font.setFontMode(1);
  font.setForegroundColor(1);
  font.drawUTF8(0, BASELINE, argv[1]);
  printf("%d %d\n", W, H);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) putchar(canvas.getPixel(x, y) ? 1 : 0);
  return 0;
}
