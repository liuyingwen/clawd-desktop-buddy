// Theme 2, 午夜: neon on a night floor. Its own animations are short (spark 1.2 s, sky 3 s), so previews loop.
#include "theme.h"

#define INK     RGB(6, 7, 16)
#define PANEL   RGB(13, 14, 28)
#define GRID    RGB(40, 42, 78)
#define PLATE   RGB(28, 29, 54)
#define WASH    RGB(72, 48, 0)
#define CYAN    RGB(56, 228, 255)
#define MAGENTA RGB(255, 70, 180)
#define LIME    RGB(120, 255, 96)
#define AMBER   RGB(255, 196, 40)
#define PAPER   RGB(232, 236, 255)
#define RED     RGB(255, 64, 80)
#define SLATE   RGB(150, 152, 196)
#define DUSK    RGB(124, 126, 180)
#define MOON    RGB(255, 236, 170)

// Thinner, brighter state icons than the classic set
static const uint8_t I_GEAR_A[7]  = {0x08,0x49,0x2A,0x63,0x2A,0x49,0x08};
static const uint8_t I_GEAR_B[7]  = {0x22,0x14,0x22,0x41,0x22,0x14,0x22};
static const uint8_t I_KEYS_A[7]  = {0x00,0x55,0x00,0x2A,0x00,0x7F,0x00};
static const uint8_t I_KEYS_B[7]  = {0x00,0x2A,0x00,0x55,0x00,0x7F,0x00};
static const uint8_t I_ASK[7]     = {0x1C,0x22,0x02,0x0C,0x08,0x00,0x08};
static const uint8_t I_CHECK[7]   = {0x00,0x01,0x02,0x44,0x28,0x10,0x00};
static const uint8_t I_CROSS[7]   = {0x41,0x22,0x14,0x08,0x14,0x22,0x41};
static const uint8_t I_ZZZ[7]     = {0x7C,0x08,0x10,0x20,0x7C,0x00,0x00};

// Same silhouettes, rebuilt for a dark floor: frames that were black turn light,
// faces turn dark, and A adds a rim light along the top edge.
static const Skin SKINS[] = {
  {"CC", {".AAAAAAAAAA.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB."},
   RGB(232, 124, 88), RGB(255, 190, 150), 0, INK, 0, 11, 2, 8, 2, 3, 7, 2, EYES_SQUARE},
  {"CX", {"..AAAAAAAA..",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          "..BBBBBBBB.."},
   PAPER, CYAN, RGB(24, 26, 52), CYAN, 0, 11, 3, 7, 2, 3, 7, 2, EYES_SQUARE, true},
  {"OC", {".AAAAAAAAAA.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BBBBBBBBBB."},
   RGB(190, 195, 220), PAPER, INK, PAPER, 0, 11, 3, 7, 2, 3, 7, 2, EYES_SQUARE},
  {"PI", {"AAAAAAAAA...",
          "BBBFFFFBB...",
          "BBBFFFFBB...",
          "BBBFFFFBB...",
          "BBBBBB...BBB",
          "BBBBBB...BBB",
          "BBB......BBB"},
   RGB(200, 180, 255), PAPER, RGB(24, 16, 48), MAGENTA, -1, -1, 0, 9, 3, 4, -1, 1, EYES_SQUARE},
  {"DS", {"..........BB",
          "..BBBBB..BB.",
          ".BBBBBBB.BB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBB..",
          ".AAAAAAA....",
          "..AAAAAA...."},
   RGB(96, 130, 255), RGB(190, 205, 255), 0, 0xFFFF, 0, -1, 2, 6, 2, 2, -1, 2, EYES_SQUARE},
  {"WB", {".B........B.",
          ".BB......BB.",
          ".AAAAAAAAAA.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BBBBBBBBBB."},
   RGB(20, 225, 185), RGB(150, 255, 230), RGB(4, 50, 44), RGB(150, 255, 230), 0, 11, 3, 7, 2, 3, 7, 3, EYES_SLASH},
};
static_assert(sizeof(SKINS) / sizeof(SKINS[0]) == SKIN_COUNT, "every theme carries all agents");

// A dim dot grid. Waiting pulses a coloured wash instead of flashing the whole cell,
// so the amber icon and the amber followers never melt into the floor.
static void cellBg(int16_t x, int16_t y, int16_t w, int16_t h, Mood m, unsigned long now) {
  canvas.fillRect(x, y, w, h, (m == MOOD_WAITING && (now / 500) % 2) ? T->pal.waitFlash : T->pal.bg);
  for (int16_t gy = y + 8; gy < y + h; gy += 16) {
    for (int16_t gx = x + 8; gx < x + w; gx += 16) canvas.writePixel(gx, gy, GRID);
  }
}

// A light tube: bright core, falling rows of glow on either side, and a spark while busy.
static void tube(int16_t x, int16_t y, int16_t w, int16_t h, Mood m, unsigned long now) {
  uint16_t c = T->pal.state[m < MOOD_UNKNOWN ? m : MOOD_IDLE];
  uint16_t mid = (c >> 1) & 0x7BEF, low = (c >> 2) & 0x39E7;
  canvas.fillRect(x, y - 2, w, 1, low);
  canvas.fillRect(x, y - 1, w, 1, mid);
  canvas.fillRect(x, y, w, h, c);
  canvas.fillRect(x, y + h, w, 1, mid);
  if (m == MOOD_WORKING || m == MOOD_THINKING) {
    const int16_t spark = 16;
    canvas.fillRect(x + (w - spark) * (long)(now % 1200) / 1200, y, spark, h, PAPER);
  }
}

// Nobody connected: Clawd asleep under a moon, stars twinkling, z's drifting up.
static const struct { uint8_t x, y, phase; } STARS[] = {
  {14, 22, 0}, {38, 58, 2}, {61, 14, 5}, {84, 40, 1}, {108, 18, 4}, {131, 66, 7}, {150, 30, 3},
  {214, 92, 6}, {226, 44, 2}, {22, 96, 5}, {70, 84, 3}, {196, 112, 1}, {12, 134, 7}, {228, 136, 4},
};

static void idleScreen(unsigned long now) {
  canvas.fillRect(0, 0, 240, 240, INK);
  for (const auto& s : STARS) {
    uint8_t beat = (now / 375 + s.phase) % 8;   // 3 s
    canvas.writePixel(s.x, s.y, beat < 4 ? PAPER : GRID);
    if (beat == 0) {
      canvas.drawFastHLine(s.x - 1, s.y, 3, PAPER);
      canvas.drawFastVLine(s.x, s.y - 1, 3, PAPER);
    }
  }
  canvas.fillCircle(186, 44, 20, MOON);         // crescent: the night cuts the moon
  canvas.fillCircle(176, 38, 18, INK);

  const int16_t ground = 192, u = 10;
  canvas.drawFastHLine(0, ground, 240, GRID);
  canvas.drawFastHLine(0, ground + 1, 240, RGB(20, 21, 40));
  drawCreature("CC", 0, MOOD_SLEEPING, 0, false, (240 - 12 * u) / 2, ground, u, now);

  for (uint8_t i = 0; i < 3; i++) {             // each z rises and fades over 3 s, a beat apart
    uint16_t t = (now + i * 1000) % 3000;
    canvas.setTextSize(i + 1);
    canvas.setTextColor(t < 2000 ? CYAN : GRID);
    canvas.setCursor(166 + i * 10 + t / 300, 128 - i * 4 - t / 60);
    canvas.print('z');
  }
  canvas.setTextSize(1);
  drawLabelCentered("等待连接", ground + 24, SLATE);
}

// CRT feel: every fourth row at 3/4 brightness, straight on the frame buffer. 14400 pixels.
static void scanlines(unsigned long) {
  uint16_t* row = canvas.getBuffer() + 3 * 240;
  for (int16_t r = 3; r < 240; r += 4, row += 4 * 240) {
    for (int16_t i = 0; i < 240; i++) row[i] = ((row[i] >> 1) & 0x7BEF) + ((row[i] >> 2) & 0x39E7);
  }
}

const Theme THEME_NOIR = {
  2, "noir", "午夜", "深夜霓虹，暗场里只亮状态", 0x38E4FF, 1,
  {
    PANEL, INK, WASH, RED,                 // bg gap waitFlash errorFrame
    PLATE, PAPER, PAPER, CYAN, CYAN,       // plate tileBorder text tag more
    INK, CYAN, RGB(64, 66, 110),           // pagerBg pagerOn pagerOff
    SLATE, PANEL,                          // smoke sleepTarget
    PLATE, AMBER,                          // toastBg toastText
    {SLATE, CYAN, LIME, AMBER, PAPER, RED, DUSK},
    {RGB(255, 140, 60), CYAN, MAGENTA, RGB(170, 130, 255), RGB(210, 170, 110), LIME},
    {CYAN, MAGENTA, LIME, AMBER, PAPER, RGB(170, 130, 255)},
  },
  SKINS, KNOT_LOGO,
  {I_GEAR_A, I_GEAR_B, I_KEYS_A, I_KEYS_B, I_ASK, I_CHECK, I_CROSS, I_ZZZ},
  {cellBg, tube, idleScreen, scanlines},
};
