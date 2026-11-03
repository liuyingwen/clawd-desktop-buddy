// Theme 1: the original cream desk. Every value is what the firmware drew before themes.
#include "theme.h"

static const uint8_t ICON_GEAR_A[7]  = {0x08,0x5D,0x3E,0x77,0x3E,0x5D,0x08};
static const uint8_t ICON_GEAR_B[7]  = {0x22,0x1C,0x3E,0x36,0x3E,0x1C,0x22};
static const uint8_t ICON_KEYS_A[7]  = {0x00,0x7F,0x55,0x7F,0x6B,0x7F,0x00};
static const uint8_t ICON_KEYS_B[7]  = {0x00,0x7F,0x6B,0x7F,0x55,0x7F,0x00};
static const uint8_t ICON_ASK[7]     = {0x3E,0x63,0x03,0x0E,0x08,0x00,0x08};
static const uint8_t ICON_CHECK[7]   = {0x00,0x01,0x03,0x46,0x6C,0x38,0x10};
static const uint8_t ICON_CROSS[7]   = {0x63,0x77,0x3E,0x1C,0x3E,0x77,0x63};
static const uint8_t ICON_ZZZ[7]     = {0x7F,0x03,0x06,0x0C,0x18,0x30,0x7F};

// The Codex logo; themes may share it
const uint32_t KNOT_LOGO[30] = {
  0x000FC000,0x003FE000,0x00707FC0,0x00E07FE0,0x00C1E070,
  0x01838038,0x038F0418,0x0F8C1F0C,0x0D8C7BCC,0x198CE0EC,
  0x398FF07C,0x318F3C1C,0x318C0F0E,0x318C0F86,0x31CC0DE3,
  0x30FC0C63,0x187C0C63,0x1C1E1C63,0x0E073C63,0x0F83FC66,
  0x0DE3CC66,0x0C7F0C7C,0x0C1C0C78,0x06003C70,0x0700F040,
  0x0383C0C0,0x01FF8180,0x007F8780,0x0001FE00,0x00007800
};

#define BLACK 0x0000
#define WHITE 0xFFFF

static const Skin SKINS[] = {
  // First entry is also the fallback; unknown sources keep a rotating colour
  {"CC", {".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB."},
   RGB(218, 119, 86), 0, 0, BLACK, 0, 11, 2, 8, 2, 3, 7, 2, EYES_SQUARE},
  {"CX", {"..BBBBBBBB..",      // little white buddy wearing the knot as a hat
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          "..BBBBBBBB.."},
   RGB(16, 16, 16), 0, WHITE, BLACK, 0, 11, 3, 7, 2, 3, 7, 2, EYES_SQUARE, true},
  {"OC", {".BBBBBBBBBB.",      // hollow black frame
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BBBBBBBBBB."},
   RGB(16, 16, 16), 0, RGB(235, 235, 235), BLACK, 0, 11, 3, 7, 2, 3, 7, 2, EYES_SQUARE},
  {"PI", {"BBBBBBBBB...",      // pixel P whose counter is a single eye, i as a leg
          "BBBFFFFBB...",
          "BBBFFFFBB...",
          "BBBFFFFBB...",
          "BBBBBB...BBB",
          "BBBBBB...BBB",
          "BBB......BBB"},
   RGB(40, 40, 48), 0, WHITE, BLACK, -1, -1, 0, 9, 3, 4, -1, 1, EYES_SQUARE},
  {"DS", {"..........BB",      // whale in profile, tail up
          "..BBBBB..BB.",
          ".BBBBBBB.BB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBB..",
          ".AAAAAAA....",
          "..AAAAAA...."},
   RGB(77, 107, 254), RGB(190, 205, 255), 0, WHITE, 0, -1, 2, 6, 2, 2, -1, 2, EYES_SQUARE},
  {"WB", {".B........B.",      // cat with a darker face
          ".BB......BB.",
          ".BBBBBBBBBB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BBBBBBBBBB."},
   RGB(13, 200, 165), 0, RGB(8, 120, 100), WHITE, 0, 11, 3, 7, 2, 3, 7, 3, EYES_SLASH},
};
static_assert(sizeof(SKINS) / sizeof(SKINS[0]) == SKIN_COUNT, "every theme carries all agents");

#define INK     BLACK
#define CREAM   RGB(255, 241, 214)
#define DIVIDER RGB(40, 34, 30)
#define YELLOW  RGB(255, 205, 0)
#define RED     RGB(225, 30, 30)
#define SLEEPY  RGB(70, 70, 76)
#define ORANGE  RGB(240, 120, 30)
#define TEAL    RGB(30, 170, 170)
#define PINK    RGB(235, 90, 150)
#define PURPLE  RGB(130, 90, 210)
#define BROWN   RGB(150, 110, 60)
#define MOSS    RGB(90, 140, 40)

const Theme THEME_CLASSIC = {
  1, "classic", "经典", "奶油色桌面，小队的原始模样", 0xDA7756, 1, 4, 0,   // id key name tagline glow minFw perPage pageMs
  {
    CREAM, DIVIDER, YELLOW, RED,          // bg gap waitFlash errorFrame
    INK, INK, INK, INK, INK,              // plate tileBorder text tag more
    DIVIDER, WHITE, SLEEPY,               // pagerBg pagerOn pagerOff
    SLEEPY, BLACK,                        // smoke sleepTarget
    INK, YELLOW,                          // toastBg toastText
    {RGB(150, 150, 150), RGB(40, 110, 240), RGB(30, 170, 70), YELLOW, WHITE, RED, SLEEPY},
    {ORANGE, TEAL, PINK, PURPLE, BROWN, MOSS},
    {ORANGE, TEAL, PINK, PURPLE, BROWN, MOSS},
  },
  SKINS, KNOT_LOGO,
  {ICON_GEAR_A, ICON_GEAR_B, ICON_KEYS_A, ICON_KEYS_B, ICON_ASK, ICON_CHECK, ICON_CROSS, ICON_ZZZ},
  {nullptr, nullptr, nullptr, nullptr},
};
