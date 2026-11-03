// Everything a theme may change. Adding one takes one theme_*.cpp and one entry in THEME_TABLE.
#pragma once
#include <Adafruit_GFX.h>

// Ids are append-only and never reused: {"cfg":{"theme":N}} and NVS carry them. THEMES[0] is theme 1.
#define THEME_TABLE(X) X(THEME_CLASSIC) X(THEME_NOIR)

#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

enum Mood {
  MOOD_IDLE, MOOD_THINKING, MOOD_WORKING, MOOD_WAITING,
  MOOD_DONE, MOOD_ERROR, MOOD_SLEEPING, MOOD_UNKNOWN
};

enum EyeStyle { EYES_SQUARE, EYES_SLASH };

// Each agent's logo as a character. rows is the 12x7 torso: B body, A accent,
// F face, anything else transparent. Columns below are in the same grid.
// Sleeping draws only rows 0/2/4/6, so a sprite must still read with those alone.
struct Skin {
  const char* src;
  const char* rows[7];
  uint16_t body, accent, face, eye;
  int8_t armL, armR;      // -1 = no arm
  int8_t legL, legR;
  uint8_t legW;
  int8_t eyeL, eyeR;      // eyeR -1 = single eye
  int8_t eyeY;
  EyeStyle eyes;
  bool knotHat;
};
#define SKIN_COUNT 6      // same agents in the same order in every theme

// One role per field: nothing borrows a state colour for an unrelated job.
struct Palette {
  uint16_t bg, gap, waitFlash, errorFrame;
  uint16_t plate, tileBorder, text, tag, more;
  uint16_t pagerBg, pagerOn, pagerOff;
  uint16_t smoke, sleepTarget;   // sleeping creatures fade halfway to sleepTarget
  uint16_t toastBg, toastText;
  uint16_t state[7];             // indexed by Mood
  uint16_t fallback[6];          // body of an unknown agent
  uint16_t confetti[6];
};

struct IconSet {          // 7x7, one byte per row, bit 6 is the leftmost pixel
  const uint8_t* gearA; const uint8_t* gearB;
  const uint8_t* keysA; const uint8_t* keysB;
  const uint8_t* ask; const uint8_t* check; const uint8_t* cross; const uint8_t* zzz;
};

// Optional. A null hook means the built-in drawing, so a theme pays only for what it replaces.
struct ThemeOps {
  void (*cellBg)(int16_t x, int16_t y, int16_t w, int16_t h, Mood m, unsigned long now);
  void (*tile)(int16_t x, int16_t y, int16_t w, int16_t h, Mood m, unsigned long now);
  void (*idleScreen)(unsigned long now);   // nobody connected
  void (*overlay)(unsigned long now);      // after the page, before the pager
};

struct Theme {
  uint8_t id;
  const char* key;
  const char* name;        // shown on the device and the page; at most 8 Chinese characters
  const char* tagline;     // the rest of this line is only for the page's manifest
  uint32_t glow;           // 0xRRGGBB
  uint8_t minFw;           // firmware build that first shipped this theme
  Palette pal;
  const Skin* skins;
  const uint32_t* knot;    // Codex hat, 30x30, one word per row, bit 29 is the leftmost pixel
  IconSet icons;
  ThemeOps ops;
};

#define DECLARE_THEME(t) extern const Theme t;
THEME_TABLE(DECLARE_THEME)
#undef DECLARE_THEME

extern const uint32_t KNOT_LOGO[30];

extern GFXcanvas16 canvas;
extern const Theme* T;

void drawCreature(const char* src, uint8_t color, Mood mood, unsigned long sinceMs, bool tinted,
                  int16_t x, int16_t feetY, uint8_t u, unsigned long now);
void drawLabelCentered(const char* text, int16_t baselineY, uint16_t col);
