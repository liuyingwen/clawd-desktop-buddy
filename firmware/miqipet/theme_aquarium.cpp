// Theme 8, 水族箱: a pixel fish tank. Each session on a page swims in its own lane with its label
// on a white tag under it; its subagents are fry in a little school beside it. States read by how a
// creature moves and by the bubble next to it. A lone session swims big over a treasure chest. Pages
// change behind a column of bubbles, and pearls on the sand show the page.
// Everything is a pure function of `now` and the session table, so no enter hook is needed. Each
// period here divides 7200 ms except the idle drift (14400 ms), which no preview shows.
#include "theme.h"

// Colours. A squad frame stays at 16 or fewer so a preview sheet keeps 4 bits per pixel.
#define W1      RGB(43, 179, 192)    // water, top band
#define W2      RGB(26, 140, 158)    // middle band
#define W3      RGB(14, 95, 117)     // deep band; also the sleep tint and dark faces
#define SURF    RGB(150, 230, 236)   // surface, light bubbles
#define FOAM    RGB(244, 251, 255)   // tags, bubbles
#define INK     RGB(11, 42, 51)      // text, dark bodies
#define SAND    RGB(232, 215, 166)
#define PEBBLE  RGB(176, 150, 98)
#define WEED    RGB(46, 158, 87)
#define WEED_DK RGB(28, 110, 64)
#define AMBER   RGB(255, 176, 0)
#define RED     RGB(255, 74, 74)
#define ORANGE  RGB(245, 124, 26)
#define BLUE    RGB(61, 107, 255)
#define MINT    RGB(110, 240, 196)
#define CU_TOP  RGB(214, 218, 226)
#define CU_MID  RGB(138, 144, 156)
#define CU_DK   RGB(66, 72, 86)
#define PLANK   RGB(138, 90, 43)
#define PINK    RGB(255, 126, 182)
#define LIME    RGB(182, 227, 90)
#define VIOLET  RGB(168, 139, 255)
#define SKY     RGB(130, 205, 255)
#define PEACH   RGB(255, 190, 150)
#define SLATE   RGB(90, 127, 140)
#define LILAC   RGB(222, 170, 255)

// Tank geometry
#define SEAM1      72        // top band ends
#define SEAM2      146       // middle band ends
#define SAND_Y     222       // sand from here down
#define LANE_TOP   4         // lanes share rows [LANE_TOP, LANE_BOTTOM)
#define LANE_BOTTOM 232
#define TAG_H      12        // a label tag: wqy12 glyphs fill it top to bottom
#define LABEL_MAX  96        // eight Chinese characters
#define SOLO_U     6
#define SOLO_FEET  160
#define CHEST_X    18
#define IDLE_MS    14400UL   // a lap of the idle drift
#define DART_MS    3600UL    // a lap of the working dart

static const uint8_t ICON_GEAR_A[7]  = {0x08,0x5D,0x3E,0x77,0x3E,0x5D,0x08};
static const uint8_t ICON_GEAR_B[7]  = {0x22,0x1C,0x3E,0x36,0x3E,0x1C,0x22};
static const uint8_t ICON_KEYS_A[7]  = {0x00,0x7F,0x55,0x7F,0x6B,0x7F,0x00};
static const uint8_t ICON_KEYS_B[7]  = {0x00,0x7F,0x6B,0x7F,0x55,0x7F,0x00};
static const uint8_t ICON_ASK[7]     = {0x3E,0x63,0x03,0x0E,0x08,0x00,0x08};
static const uint8_t ICON_CHECK[7]   = {0x00,0x01,0x03,0x46,0x6C,0x38,0x10};
static const uint8_t ICON_CROSS[7]   = {0x63,0x77,0x3E,0x1C,0x3E,0x77,0x63};
static const uint8_t ICON_ZZZ[7]     = {0x7F,0x03,0x06,0x0C,0x18,0x30,0x7F};

// The cast in the water. No body shares its colour with that creature's eyes, so error can repaint
// the body red pixel by pixel. OC is a hollow frame: the water shows through it.
static const Skin SKINS[] = {
  {"CC", {".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB."},
   ORANGE, 0, 0, INK, 0, 11, 2, 8, 2, 3, 7, 2, EYES_SQUARE},
  {"CX", {"..BBBBBBBB..",      // white robot with blue eyes, the knot as a hat
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          "..BBBBBBBB.."},
   INK, 0, FOAM, BLUE, 0, 11, 3, 7, 2, 3, 7, 2, EYES_SQUARE, true},
  {"OC", {".BBBBBBBBBB.",      // hollow dark frame; the eyes sit in dark sockets
          ".B........B.",
          ".B.FF..FF.B.",
          ".B.FF..FF.B.",
          ".B........B.",
          ".B........B.",
          ".BBBBBBBBBB."},
   INK, 0, INK, FOAM, 0, 11, 3, 7, 2, 3, 7, 2, EYES_SQUARE},
  {"PI", {"BBBBBBBBB...",      // pixel P whose counter holds a single eye
          "BBBFFFFBB...",
          "BBBFFFFBB...",
          "BBBFFFFBB...",
          "BBBBBB...BBB",
          "BBBBBB...BBB",
          "BBB......BBB"},
   INK, 0, FOAM, BLUE, -1, -1, 0, 9, 3, 4, -1, 1, EYES_SQUARE},
  {"DS", {"..........BB",      // whale in profile, tail up, pale belly
          "..BBBBB..BB.",
          ".BBBBBBB.BB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBB..",
          ".AAAAAAA....",
          "..AAAAAA...."},
   BLUE, FOAM, 0, FOAM, 0, -1, 2, 6, 2, 2, -1, 2, EYES_SQUARE},
  {"WB", {".B........B.",      // mint cat with a deep-water face
          ".BB......BB.",
          ".BBBBBBBBBB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BBBBBBBBBB."},
   MINT, 0, W3, FOAM, 0, 11, 3, 7, 2, 3, 7, 3, EYES_SLASH},
  {"CU", {"....AAAA....",      // isometric cube: brightest top, mid left, dark right
          "..AAAAAAAA..",
          ".BAAAAAAAAF.",
          ".BBBAAAAFFF.",
          ".BBBBBFFFFF.",
          ".BBBBBFFFFF.",
          "...BBBFFF..."},
   CU_MID, CU_TOP, CU_DK, FOAM, 0, 11, 3, 7, 2, 3, 7, 4, EYES_SQUARE},
};
static_assert(sizeof(SKINS) / sizeof(SKINS[0]) == SKIN_COUNT, "every theme carries all agents");

// ── Primitives ──────────────────────────────────────────────────
// OX: x origin of the page being drawn. This theme's own transition draws at 0; the offset is only
// for a caller that slides drawPage().
static int16_t OX = 0;

static inline void rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) { canvas.fillRect(x + OX, y, w, h, c); }

static void glyph(int16_t x, int16_t y, const uint8_t* rows, uint8_t u, uint16_t c) {
  for (uint8_t r = 0; r < 7; r++) {
    for (uint8_t k = 0; k < 7; k++) {
      if (rows[r] & (0x40 >> k)) rect(x + k * u, y + r * u, u, u, c);
    }
  }
}

// A square bubble with its corners knocked off
static void blob(int16_t x, int16_t y, int16_t s, uint16_t c) {
  if (s < 3) { rect(x, y, s, s, c); return; }
  rect(x + 1, y, s - 2, s, c);
  rect(x, y + 1, s, s - 2, c);
}

// A hollow bubble with a glint
static void ring(int16_t x, int16_t y, int16_t s, uint16_t c) {
  rect(x + 1, y, s - 2, 1, c);
  rect(x + 1, y + s - 1, s - 2, 1, c);
  rect(x, y + 1, 1, s - 2, c);
  rect(x + s - 1, y + 1, 1, s - 2, c);
  rect(x + s - 3, y + 2, 1, 1, c);
}

static void text5x7(const char* s, int16_t x, int16_t y, uint8_t size, uint16_t c) {
  canvas.setTextSize(size);
  canvas.setTextColor(c);
  canvas.setCursor(x + OX, y);
  canvas.print(s);
  canvas.setTextSize(1);
}

// 0..range..0 over one period: a lap from one glass to the other and back
static int16_t lap(unsigned long t, unsigned long period, int16_t range) {
  unsigned long p = t % period, half = period / 2;
  return p < half ? (long)p * range / half : (long)(period - p) * range / half;
}

// Repaints one colour inside a box of the frame: an erroring creature turns red
static void repaint(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t from, uint16_t to) {
  x += OX;
  int16_t x1 = min<int16_t>(x + w, DISP_W), y1 = min<int16_t>(y + h, DISP_H);
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  uint16_t* buf = canvas.getBuffer();
  for (int16_t py = y; py < y1; py++) {
    uint16_t* row = buf + py * DISP_W;
    for (int16_t px = x; px < x1; px++) {
      if (row[px] == from) row[px] = to;
    }
  }
}

// ── The tank ────────────────────────────────────────────────────

// Seam between two bands: teeth of the upper colour hang into the lower one
static void seam(int16_t y, uint16_t upper) {
  for (int16_t x = 0; x < DISP_W; x += 8) {
    rect(x, y, 4, 3, upper);
    rect(x + 5, y + 3, 2, 2, upper);
  }
}

static const int8_t WAVE[6] = {0, 1, 2, 2, 1, 0};

static void surface(unsigned long now) {
  rect(0, 0, DISP_W, 3, SURF);
  uint8_t shift = (now / 200) % 6;                      // the swell runs right, 1200 ms a crest
  for (int16_t x = 0, i = 0; x < DISP_W; x += 4, i++) {
    int8_t h = WAVE[(i + 6 - shift) % 6];
    if (h) rect(x, 3, 4, h, SURF);
  }
  for (uint8_t i = 0; i < 6; i++) {                     // glints twinkle along the surface
    if ((now / 300 + i * 3) % 4 == 0) rect(14 + i * 41, 1, 5, 1, FOAM);
  }
}

// Seaweed: segments of 3 rows. A wave runs up each strand, swinging more towards the tip, with a
// leaf every fourth segment; a cycle is 3600 ms.
static const int8_t SWAY[12] = {0, 1, 2, 3, 3, 2, 0, -1, -2, -3, -3, -2};
static const struct { int16_t x, h; uint16_t c; uint8_t phase; } STRANDS[] = {
  {12, 99, WEED_DK, 5}, {4, 150, WEED, 0}, {222, 84, WEED_DK, 2}, {214, 51, WEED, 9}, {231, 135, WEED, 7}};

static void seaweed(unsigned long now) {
  uint8_t step = (now / 300) % 12;
  for (const auto& s : STRANDS) {
    uint8_t segs = s.h / 3;
    for (uint8_t k = 0; k < segs; k++) {
      int16_t y = SAND_Y - 3 * (k + 1);
      int16_t off = SWAY[(step + s.phase + 24 - (k / 3) % 12) % 12] * min<int16_t>(k, 18) / 18;
      bool tip = k + 2 >= segs;
      rect(s.x + off + (tip ? 1 : 0), y, tip ? 2 : 4, 3, s.c);
      if (k % 4 == 3 && !tip) {                          // leaves, alternating sides, pointing up
        bool right = (k / 4) % 2;
        int16_t lx = right ? s.x + off + 4 : s.x + off - 2;
        rect(lx, y + 1, 2, 2, s.c);
        rect(right ? lx + 2 : lx - 1, y - 1, 1, 2, s.c);
      }
    }
  }
}

// A few rounded stones round the roots on the right
static void stone(int16_t x, int16_t y, int16_t w, int16_t h) {
  rect(x + 2, y, w - 4, 1, PEBBLE);
  rect(x + 1, y + 1, w - 2, 1, PEBBLE);
  rect(x, y + 2, w, h - 2, PEBBLE);
  rect(x + 2, y + 1, w / 3, 1, SAND);                   // light from the surface
  rect(x + 1, y + h - 1, w - 2, 1, INK);                // its shadow on the sand
}

static void rocks() {
  stone(186, SAND_Y - 11, 18, 12);
  stone(201, SAND_Y - 6, 11, 7);
}

static const struct { int16_t x, y, w, h; } PEBBLES[] = {
  {30, 226, 6, 3}, {58, 231, 4, 2}, {84, 225, 3, 2}, {120, 229, 7, 3}, {150, 225, 4, 2},
  {176, 230, 5, 3}, {205, 226, 4, 2}, {98, 235, 3, 2}, {8, 234, 4, 2}};

static void sand() {
  rect(0, SAND_Y, DISP_W, DISP_H - SAND_Y, SAND);
  rect(0, SAND_Y, DISP_W, 1, PEBBLE);
  for (int16_t x = 6; x < DISP_W; x += 14) rect(x, SAND_Y + 1, 5, 1, PEBBLE);   // ripples on the bed
  for (const auto& p : PEBBLES) rect(p.x, p.y, p.w, p.h, PEBBLE);
}

// Bubbles rising behind everything, each column on its own 3600 or 1800 ms climb
static const struct { int16_t x; uint16_t phase; uint8_t size; bool hollow; uint16_t period; } RISERS[] = {
  {28, 0, 2, false, 3600}, {66, 1300, 4, true, 3600}, {104, 2500, 2, false, 1800}, {141, 700, 3, false, 3600},
  {178, 2100, 5, true, 3600}, {203, 400, 2, false, 1800}, {45, 1900, 3, false, 1800}, {214, 3000, 2, false, 3600}};

static void risers(unsigned long now) {
  for (const auto& b : RISERS) {
    unsigned long t = (now + b.phase) % b.period;
    int16_t y = SAND_Y - 6 - (int32_t)t * (SAND_Y - 12) / b.period;
    int16_t x = b.x + ((now + b.phase) / 600) % 2;
    if (b.hollow) ring(x, y, b.size, FOAM);
    else rect(x, y, b.size, b.size, (b.size > 2) ? FOAM : SURF);
  }
}

static void chest() {
  const int16_t x = CHEST_X, y = SAND_Y - 12, w = 28;
  rect(x, y, w, 18, INK);                               // outline
  rect(x + 1, y + 1, w - 2, 16, PLANK);
  rect(x + 1, y + 6, w - 2, 1, INK);                    // the lid's seam
  rect(x + 4, y + 1, 2, 16, PEBBLE);                    // bands
  rect(x + w - 6, y + 1, 2, 16, PEBBLE);
  rect(x + w / 2 - 2, y + 5, 4, 5, AMBER);              // lock
  rect(x + w / 2 - 1, y + 7, 2, 2, INK);
  rect(x + 6, y - 2, 3, 2, AMBER);                      // gold peeking out of the lid
  rect(x + 16, y - 1, 2, 1, AMBER);
}

static void scenery(unsigned long now, bool withChest) {
  rect(0, 0, DISP_W, SEAM1, W1);
  rect(0, SEAM1, DISP_W, SEAM2 - SEAM1, W2);
  rect(0, SEAM2, DISP_W, SAND_Y - SEAM2, W3);
  seam(SEAM1, W1);
  seam(SEAM2, W2);
  surface(now);
  sand();
  seaweed(now);
  rocks();
  if (withChest) chest();
  risers(now);
}

// Pearls on the sand at the bottom right, one per page; another page's pearl blinks in an
// alerting session's colour
static void pearls(uint8_t pages, uint8_t active, unsigned long now) {
  if (pages < 2) return;
  int16_t x = DISP_W - 6 - pages * 9;
  for (uint8_t p = 0; p < pages; p++, x += 9) {
    uint16_t c = PEBBLE;
    for (uint8_t i = p * T->perPage; i < sessionCount && i < (p + 1) * T->perPage; i++) {
      if (p != active && isAlerting(sessions[i]) && (now / 250) % 2) c = stateColor(sessions[i].mood);
    }
    if (p == active) {
      blob(x - 1, SAND_Y + 10, 7, INK);
      blob(x, SAND_Y + 11, 5, FOAM);
    } else {
      blob(x, SAND_Y + 11, 5, c);
    }
  }
}

// ── A creature in its lane ──────────────────────────────────────

static Mood moodOf(const Session& s) { return s.mood == MOOD_UNKNOWN ? MOOD_IDLE : s.mood; }

static int16_t hatAbove(uint8_t u) { return 5 * u - u / 2; }   // rows the Codex hat rises above the torso

// Where one session's creature, school, bubble and tag go this frame
struct Fish {
  const Session* s;
  Mood m;
  uint8_t u, fu, iu;        // creature, fry and bubble-icon units
  int16_t x, feet;          // creature bottom-left; feet is one past its lowest row
  int16_t top, bottom;      // its lane
  int8_t dir;               // travel: +1 right, -1 left, 0 holding still
  bool schoolRight;         // fry on the right (trailing a fish that swims left)
  bool tagBeside;           // waiting in a tight lane: the tag sits right of the creature
  bool noHop;               // a hop would leave the lane
  bool solo;
  int16_t tagX, tagY, tagW;
  int16_t left, right;      // horizontal extent of everything it draws
};

static uint8_t laneUnit(uint8_t count) { return count <= 1 ? SOLO_U : count == 2 ? 5 : count == 3 ? 4 : 3; }

// Room beside a creature for the school on one side and the state bubble on the other
static int16_t reserve(uint8_t u) {
  uint8_t fu = u >= 5 ? 2 : 1, iu = u >= 5 ? 3 : 2;
  return max<int16_t>(2 * (12 * fu + 1), 9 * iu) + 3;
}

static int16_t tagWidth(const Session& s, int16_t maxLabel, char* label, size_t size) {
  int16_t lw = fitLabel(label, size, labelOf(s), maxLabel);
  return 3 + (s.src[0] ? 14 : 0) + lw + 3;
}

static Fish place(const Session& s, uint8_t idx, uint8_t count, unsigned long now) {
  Fish f = {};
  f.s = &s;
  f.m = moodOf(s);
  f.solo = count <= 1;
  uint8_t base = laneUnit(count);
  f.u = f.m == MOOD_WAITING ? base + 1 : base;
  f.fu = base >= 5 ? 2 : 1;
  f.iu = base >= 5 ? 3 : 2;
  if (f.solo) {
    f.top = 0;
    f.bottom = SAND_Y;
  } else {
    int16_t h = (LANE_BOTTOM - LANE_TOP) / count;
    f.top = LANE_TOP + idx * h;
    f.bottom = f.top + h;
  }
  const int16_t R = reserve(base);
  const int16_t lift = max<uint8_t>(f.u, 2);
  const bool hat = skinFor(s.src).knotHat;
  char label[sizeof(Session::title)];
  f.tagW = tagWidth(s, f.solo ? 132 : LABEL_MAX, label, sizeof(label));

  // Vertical: the tag under the creature, unless a waiting creature only fits with it alongside
  int16_t under = f.solo ? SOLO_FEET : f.bottom - TAG_H - 1;
  int16_t need = 9 * f.u + (hat ? hatAbove(f.u) : 0) + lift;
  f.feet = under;
  if (f.m == MOOD_WAITING && under - f.top < need) {
    f.tagBeside = true;
    f.feet = f.bottom;
    f.noHop = f.feet - f.top < need;
  }
  if (f.m == MOOD_SLEEPING) f.feet = f.solo ? SAND_Y + 4 : under - 2;   // settles on the sand or a sandbank

  // Horizontal: a lap of the lane between the reserves, or held still
  const int16_t xmin = 4 + R, range = max<int16_t>(DISP_W - 4 - R - 12 * base - xmin, 0);
  unsigned long drift = now + idx * (IDLE_MS * 3 / 8);
  switch (f.m) {
    case MOOD_WORKING: {   // darts the length of the lane
      unsigned long t = now + idx * (DART_MS / 4);
      f.x = xmin + lap(t, DART_MS, range);
      f.dir = t % DART_MS < DART_MS / 2 ? 1 : -1;
      break;
    }
    case MOOD_IDLE:
    case MOOD_DONE:
      f.x = xmin + lap(drift, IDLE_MS, range);
      f.dir = drift % IDLE_MS < IDLE_MS / 2 ? 1 : -1;
      break;
    case MOOD_WAITING:     // swims to the front, in the middle of the glass
      if (f.tagBeside) {
        int16_t total = R + 12 * f.u + 4 + max<int16_t>(f.tagW, 9 * f.iu);
        f.x = (DISP_W - total) / 2 + R;
      } else {
        f.x = (DISP_W - 12 * f.u) / 2;
      }
      break;
    case MOOD_SLEEPING:
      if (f.solo) { f.x = (DISP_W - 12 * f.u) / 2; break; }
      // fall through: rests where it was when it fell asleep
    default:               // thinking, error: hold where the drift had it when the state began
      f.x = xmin + lap(s.moodSinceMs + idx * (IDLE_MS * 3 / 8), IDLE_MS, range);
      break;
  }
  f.schoolRight = f.dir < 0;

  if (f.tagBeside) {
    f.tagX = f.x + 12 * f.u + 4;
    f.tagY = f.feet - TAG_H;
  } else {
    f.tagX = constrain(f.x + 6 * f.u - f.tagW / 2, 2, DISP_W - 2 - f.tagW);
    f.tagY = f.m == MOOD_SLEEPING ? f.feet + (f.solo ? 0 : 2) + 1 : under + 1;
    if (f.solo && f.m == MOOD_SLEEPING) f.tagY = DISP_H - TAG_H - 1;
  }
  // Extent: the creature with its error ring, the school, the state bubble, a working trail
  int16_t padL = 6, padR = 6;
  int16_t schoolW = s.followerCount ? 2 * (12 * f.fu + 1) + 7 : 0;
  (f.schoolRight ? padR : padL) = max<int16_t>(f.schoolRight ? padR : padL, schoolW);
  if (f.m == MOOD_THINKING || f.m == MOOD_ERROR || f.m == MOOD_DONE || f.m == MOOD_WAITING) {
    int16_t& side = f.dir < 0 ? padL : padR;
    side = max<int16_t>(side, 9 * f.iu + 6);
  }
  if (f.m == MOOD_WORKING) (f.dir > 0 ? padL : padR) = max<int16_t>(schoolW, 40);
  f.left = min<int16_t>(f.x - padL, f.tagX);
  f.right = max<int16_t>(f.x + 12 * f.u + padR, f.tagX + f.tagW);
  return f;
}

// The torso's top row this frame, before drawCreature's own hop
static int16_t torsoTop(const Fish& f) { return f.feet - (f.m == MOOD_SLEEPING ? 4 : 9) * f.u; }

static void drawTag(const Fish& f, unsigned long now) {
  const Session& s = *f.s;
  bool flash = f.m == MOOD_WAITING && isAlerting(s) && (now / 300) % 2;
  uint16_t bg = flash || (f.m == MOOD_WAITING && !isAlerting(s)) ? AMBER : FOAM;
  rect(f.tagX, f.tagY, f.tagW, TAG_H, bg);
  if (f.m == MOOD_ERROR) rect(f.tagX, f.tagY, 2, TAG_H, RED);
  int16_t x = f.tagX + 3;
  if (s.src[0]) {
    text5x7(s.src, x, f.tagY + 3, 1, f.m == MOOD_ERROR ? RED : bg == AMBER ? INK : W2);
    x += 14;
  }
  drawLabel(labelOf(s), x + OX, f.tagY + 10, f.tagW - (x - f.tagX) - 3, INK);
}

// Subagents: fry in a two-by-two school beside the creature, "+N" in the last place when more come
static void school(const Fish& f, unsigned long now) {
  const Session& s = *f.s;
  uint8_t n = s.followerCount;
  if (!n) return;
  const uint8_t fu = f.fu;
  const int16_t pitch = 12 * fu + 1, rowH = 9 * fu + 5 * fu;
  uint8_t shown = n <= 4 ? n : 3;
  const int16_t gap = f.m == MOOD_ERROR ? 7 : 3;         // clear of the warning ring
  int16_t near = f.schoolRight ? f.x + 12 * f.u + gap : f.x - gap - 12 * fu;
  int16_t step = f.schoolRight ? pitch : -pitch;
  int16_t low = f.m == MOOD_SLEEPING ? f.feet : f.feet - 1;
  for (uint8_t i = 0; i < 4; i++) {
    int16_t fx = near + (i % 2) * step, feet = (n == 1) ? low - rowH / 2 : low - (i / 2 == 0 ? rowH : 0);
    if (i == 3 && n > 4) {
      char more[5];
      snprintf(more, sizeof(more), "+%u", n - shown);
      text5x7(more, fx + (12 * fu - 6 * (int16_t)strlen(more)) / 2, feet - 5 * fu - 3, 1, FOAM);
      break;
    }
    if (i >= shown) break;
    const Follower& fo = s.followers[i];
    unsigned long age = now - fo.bornMs;
    int16_t drop = age < 400 ? (400 - age) / 40 : 0;   // darts in from above
    drawCreature(s.src, s.color, fo.mood, fo.moodSinceMs, fo.mood == MOOD_WAITING, fx + OX, feet - drop, fu,
                 now + i * 110);
  }
}

// The state bubble: a white thought with a gear, an amber "?", a red "×", a check when done
static void stateBubble(const Fish& f, unsigned long now) {
  const uint8_t* icon;
  uint16_t bg = FOAM, ink;
  switch (f.m) {
    case MOOD_THINKING: icon = iconFor(MOOD_THINKING, now); ink = BLUE; break;
    case MOOD_WAITING:  icon = ICON_ASK; bg = AMBER; ink = INK; break;
    case MOOD_ERROR:    icon = ICON_CROSS; bg = RED; ink = FOAM; break;
    case MOOD_DONE:     icon = ICON_CHECK; ink = WEED; break;
    default: return;
  }
  const int16_t size = 9 * f.iu;
  int16_t bx, by;
  if (f.tagBeside) {                                    // above the tag, beside the creature
    bx = f.tagX;
    by = f.tagY - size - 3;
  } else {
    bool right = !f.schoolRight || f.dir == 0;
    if (f.dir != 0) right = f.dir > 0;                  // leads a moving fish
    bx = right ? f.x + 12 * f.u + 3 : f.x - 3 - size;
    by = torsoTop(f) - (f.solo ? size / 2 : 2);
  }
  if (f.m == MOOD_WAITING || f.m == MOOD_THINKING) by -= (now / 300) % 2;   // bobs
  blob(bx, by, size, bg);
  glyph(bx + f.iu, by + f.iu, icon, f.iu, ink);
  if (f.m == MOOD_THINKING && !f.tagBeside) {           // little thought bubbles back to the head
    bool right = bx > f.x;
    rect(right ? bx - 3 : bx + size + 1, by + size, 2, 2, FOAM);
  }
}

// Working: a trail of bubbles where it has just been
static void trail(const Fish& f, uint8_t idx, unsigned long now) {
  const int16_t xmin = 4 + reserve(f.u), range = max<int16_t>(DISP_W - 4 - reserve(f.u) - 12 * f.u - xmin, 0);
  int16_t by = torsoTop(f);
  for (uint8_t k = 1; k <= 4; k++) {
    unsigned long t = now + idx * (DART_MS / 4) - k * 120;
    int16_t px = xmin + lap(t, DART_MS, range);
    int8_t dir = t % DART_MS < DART_MS / 2 ? 1 : -1;
    int16_t x = dir > 0 ? px + 2 : px + 12 * f.u - 4;
    int16_t s = k < 3 ? 3 : 2;
    rect(x, by + 3 * f.u - k * 2 + (k % 2), s, s, k < 3 ? FOAM : SURF);
  }
}

// Sleeping: small z bubbles rise from the head, 2400 ms each
static void zzz(const Fish& f, unsigned long now) {
  uint8_t size = f.solo ? 2 : 1;
  int16_t rise = f.solo ? 48 : 26;
  int16_t top = torsoTop(f);
  for (uint8_t i = 0; i < 3; i++) {
    unsigned long t = (now + i * 800) % 2400;
    int16_t x = f.x + 10 * f.u + i * 2 * size + t * 6 / 2400;
    int16_t y = top - 8 * size - t * rise / 2400;
    if (y < f.top) continue;
    text5x7("z", x, y, size, t < 1800 ? FOAM : SURF);
  }
}

// Error: eight warning bubbles orbit the creature, red and white in turn, sized with the lane
static void warnRing(const Fish& f, unsigned long now) {
  int16_t x0 = f.x - 5, y0 = torsoTop(f) - 4, w = 12 * f.u + 10, h = f.feet - y0 + 1;
  if (y0 < f.top + f.iu) { h -= f.top + f.iu - y0; y0 = f.top + f.iu; }
  int16_t per = 2 * (w + h);
  const int16_t sz = f.iu + 2;                          // 4 in a lane, 5 solo
  uint8_t step = (now / 150) % 2;
  for (uint8_t k = 0; k < 8; k++) {
    int16_t d = (per * k / 8 + per * ((now / 150) % 24) / 24) % per, px, py;   // a turn in 3600 ms
    if (d < w) { px = x0 + d; py = y0; }
    else if (d < w + h) { px = x0 + w; py = y0 + d - w; }
    else if (d < 2 * w + h) { px = x0 + w - (d - w - h); py = y0 + h; }
    else { px = x0; py = y0 + h - (d - 2 * w - h); }
    if (k % 2 == step) blob(px - sz / 2, py - sz / 2, sz, RED);
    else ring(px - sz / 2, py - sz / 2, sz + 1, FOAM);
  }
}

// Done: a burst of bubbles flies out of it for the first second
static void burst(const Fish& f, unsigned long now) {
  unsigned long t = now - f.s->moodSinceMs;
  if (t >= 1000) return;
  static const int8_t DIR[8][2] = {{8, 0}, {6, -6}, {0, -8}, {-6, -6}, {-8, 0}, {-6, 6}, {0, 8}, {6, 6}};
  int16_t cx = f.x + 6 * f.u, cy = torsoTop(f) + 4 * f.u;
  int16_t r = 4 * f.u + t * 5 * f.u / 1000;
  for (uint8_t k = 0; k < 8; k++) {
    int16_t x = cx + DIR[k][0] * r / 8, y = cy + DIR[k][1] * r / 8;
    if (y < f.top + 1 || y + 4 > f.tagY) continue;
    if (k % 2) ring(x - 2, y - 2, 5, FOAM);
    else rect(x - 1, y - 1, 3, 3, T->pal.confetti[(k / 2 + t / 200) % 6]);
  }
}

static void drawFish(const Fish& f, uint8_t idx, unsigned long now) {
  const Session& s = *f.s;
  const Skin& k = skinFor(s.src);
  int16_t top = torsoTop(f), w = 12 * f.u;

  if (f.m == MOOD_WORKING) trail(f, idx, now);
  if (f.m == MOOD_SLEEPING) {                           // a bank of sand to lie in: it keeps a dimmed sleeper readable
    int16_t y0 = f.feet - 4 * f.u - 2, bw = w + 8;
    static const uint8_t INSET[3] = {5, 2, 1};
    for (uint8_t r = 0; r < 3; r++) rect(f.x - 4 + INSET[r], y0 + r, bw - 2 * INSET[r], 1, SAND);
    rect(f.x - 4, y0 + 3, bw, f.feet - y0 - 2, SAND);
    rect(f.x - 3, f.feet + 1, bw - 2, 1, PEBBLE);
    rect(f.x + 2, y0 + 2, 3, 1, PEBBLE);
    rect(f.x + w - 3, y0 + 3, 2, 1, PEBBLE);
  }
  if (f.m == MOOD_WAITING) {                            // the glass it taps: an amber frame
    bool lit = !isAlerting(s) || (now / 300) % 2 == 0;
    int16_t fy = max<int16_t>(top - 3, f.top), fh = f.feet - fy + (f.tagBeside ? 0 : 1);
    canvas.drawRect(f.x - 3 + OX, fy, w + 6, fh, lit ? AMBER : FOAM);   // two pixels of glass
    canvas.drawRect(f.x - 2 + OX, fy + 1, w + 4, fh - (f.tagBeside ? 1 : 2), lit ? AMBER : FOAM);
    if ((now % 1200) < 300) {                           // tap marks at the corners
      rect(f.x - 6, fy - 1, 2, 1, FOAM); rect(f.x + w + 4, fy - 1, 2, 1, FOAM);
      rect(f.x - 6, fy + fh, 2, 1, FOAM); rect(f.x + w + 4, fy + fh, 2, 1, FOAM);
    }
  }

  int16_t feet = f.feet;
  if (f.noHop && (now / 250) % 2) feet += max<uint8_t>(f.u, 2);   // cancels drawCreature's hop
  if (f.m == MOOD_THINKING) feet -= (now / 600) % 2;               // hovers
  bool tint = f.m == MOOD_WAITING && isAlerting(s) && (now / 300) % 2;
  drawCreature(s.src, s.color, f.m, s.moodSinceMs, tint, f.x + OX, feet, f.u, now);
  if (f.m == MOOD_ERROR) {
    uint16_t body = strcmp(k.src, s.src) ? T->pal.fallback[s.color % 6] : k.body;
    int16_t hat = k.knotHat ? hatAbove(f.u) : 0;
    repaint(f.x - f.u, top - hat - 2, w + 2 * f.u, f.feet - top + hat + 2, body, RED);
    warnRing(f, now);
  }

  school(f, now);
  if (f.m == MOOD_SLEEPING) zzz(f, now);
  if (f.m == MOOD_DONE) burst(f, now);
  stateBubble(f, now);
  drawTag(f, now);
}

// The tank's glass flashes while a session on this page is alerting: amber for waiting, red for error
static void glass(uint8_t first, uint8_t count, unsigned long now) {
  uint16_t c = 0;
  for (uint8_t i = first; i < first + count && i < sessionCount; i++) {
    if (!isAlerting(sessions[i])) continue;
    if (sessions[i].mood == MOOD_WAITING) c = AMBER;
    else if (!c) c = RED;
  }
  if (!c || (now / 300) % 2) return;
  rect(0, 0, DISP_W, 2, c);
  rect(0, DISP_H - 2, DISP_W, 2, c);
  rect(0, 2, 2, DISP_H - 4, c);
  rect(DISP_W - 2, 2, 2, DISP_H - 4, c);
}

static void fishes(uint8_t first, uint8_t count, unsigned long now) {
  for (uint8_t i = 0; i < count; i++) drawFish(place(sessions[first + i], i, count, now), i, now);
}

// ── Hooks ───────────────────────────────────────────────────────

static void page(uint8_t first, uint8_t count, int16_t ox, int16_t, unsigned long now) {
  OX = ox;
  scenery(now, count == 1);
  fishes(first, count, now);
  pearls(pageCount(), first / T->perPage, now);
  glass(first, count, now);
  OX = 0;
}

static uint8_t onPage(uint8_t p) {
  uint8_t first = p * T->perPage;
  return sessionCount > first ? min<uint8_t>(sessionCount - first, T->perPage) : 0;
}

// A column of bubbles sweeps right to left: creatures it has passed belong to the next page.
// A creature is drawn whole on one side or not at all, so the two pages never overlap.
static void transition(uint8_t from, uint8_t to, uint16_t progress, int16_t, unsigned long now) {
  OX = 0;
  const int16_t band = 44;
  int16_t mid = DISP_W + band / 2 - (int32_t)(DISP_W + band) * progress / TRANSITION_ONE;
  uint8_t nFrom = onPage(from), nTo = onPage(to);
  rect(0, 0, DISP_W, SEAM1, W1);
  rect(0, SEAM1, DISP_W, SEAM2 - SEAM1, W2);
  rect(0, SEAM2, DISP_W, SAND_Y - SEAM2, W3);
  seam(SEAM1, W1);
  seam(SEAM2, W2);
  surface(now);
  sand();
  seaweed(now);
  rocks();
  if ((nFrom == 1 && mid > CHEST_X + 30) || (nTo == 1 && mid < CHEST_X)) chest();
  risers(now);
  for (uint8_t i = 0; i < nFrom; i++) {
    Fish f = place(sessions[from * T->perPage + i], i, nFrom, now);
    if (f.right <= mid) drawFish(f, i, now);
  }
  for (uint8_t i = 0; i < nTo; i++) {
    Fish f = place(sessions[to * T->perPage + i], i, nTo, now);
    if (f.left >= mid) drawFish(f, i, now);
  }
  // The column: four streams of bubbles racing up
  for (uint8_t c = 0; c < 4; c++) {
    int16_t cx = mid - band / 2 + c * 11 + 2;
    for (uint8_t k = 0; k < 13; k++) {
      static const uint8_t SIZE[5] = {6, 3, 8, 4, 5};
      uint8_t sz = SIZE[(k + c) % 5];
      int16_t y = (int16_t)((k * 19 + c * 7 + 400 - (now / 10) % 240) % 240) - 4;
      if (y > SAND_Y - 2) continue;
      int16_t x = cx + ((k + c) % 3) * 2 - 2;
      if (sz >= 6) ring(x, y, sz, FOAM);
      else blob(x, y, sz, (k + c) % 2 ? FOAM : SURF);
    }
  }
  pearls(pageCount(), to, now);
}

// Nobody connected: an empty tank and a sign stuck in the sand
static void emptyTank(unsigned long now) {
  OX = 0;
  scenery(now, false);
  const char* msg = "等待会话接入";
  int16_t tw = labelFont.getUTF8Width(msg), w = tw + 16, h = 22, x = (DISP_W - w) / 2, y = 182;
  rect(DISP_W / 2 - 2, y + h, 4, SAND_Y + 8 - y - h, PLANK);   // post
  rect(DISP_W / 2 - 6, SAND_Y + 6, 12, 2, PEBBLE);              // sand heaped round it
  rect(x, y, w, h, PLANK);
  rect(x + 2, y + 2, w - 4, h - 4, FOAM);
  labelFont.setForegroundColor(INK);
  labelFont.drawUTF8(x + 8, y + 16, msg);
  // A starfish and two shells on the sand
  static const uint16_t STAR[8] = {0x010, 0x038, 0x1FF, 0x0FE, 0x07C, 0x06C, 0x0C6, 0x082};
  for (uint8_t r = 0; r < 8; r++) {
    for (uint8_t c = 0; c < 9; c++) {
      if (STAR[r] & (0x100 >> c)) rect(66 + c, SAND_Y + 3 + r, 1, 1, ORANGE);
    }
  }
  blob(150, SAND_Y + 5, 6, FOAM);
  rect(151, SAND_Y + 7, 4, 1, PEBBLE);
  blob(162, SAND_Y + 9, 5, PEBBLE);
  // A lone bubble trail from the sign, so the tank is clearly alive
  for (uint8_t i = 0; i < 3; i++) {
    unsigned long t = (now + i * 600) % 1800;
    ring(x + w - 10 + (t / 300) % 2, y - 6 - t * 60 / 1800, 4 - i % 2, FOAM);
  }
}

static int16_t noPager(uint8_t) { return 0; }

// The settings notice: a white board hanging from the surface on two lines; items wrap at their gaps
static void notice(const char* text, unsigned long now) {
  char lines[3][64];
  uint8_t count = 0;
  const int16_t maxW = 176;
  const char* p = text;
  lines[0][0] = 0;
  while (*p && count < 3) {
    const char* gap = strstr(p, "  ");
    size_t len = gap ? (size_t)(gap - p) : strlen(p);
    char item[64], joined[64];
    snprintf(item, sizeof(item), "%.*s", (int)len, p);
    snprintf(joined, sizeof(joined), "%s%s%s", lines[count], lines[count][0] ? "  " : "", item);
    if (!lines[count][0] || labelFont.getUTF8Width(joined) <= maxW) {
      strlcpy(lines[count], joined, sizeof(lines[count]));
    } else if (count < 2) {
      strlcpy(lines[++count], item, sizeof(lines[count]));
    }
    p += len;
    while (*p == ' ') p++;
  }
  if (lines[count][0]) count++;
  if (!count) return;
  const int16_t w = 196, x = (DISP_W - w) / 2, y = 30;
  int16_t h = 14 * count + 12;
  // Every lane's tag sits in the bottom TAG_H rows of the lane. A board ending inside one of those
  // bands would leave half a tag peeking out, so it reaches down past that tag instead.
  uint8_t lanes = onPage(slideTo >= 0 ? (uint8_t)slideTo : currentPage);
  if (lanes >= 2) {
    const int16_t laneH = (LANE_BOTTOM - LANE_TOP) / lanes;
    for (uint8_t i = 1; i <= lanes; i++) {
      int16_t tagTop = LANE_TOP + i * laneH - TAG_H, tagEnd = LANE_TOP + i * laneH;
      if (y + h > tagTop && y + h < tagEnd) h = tagEnd - y;
    }
  }
  rect(x + 20, 0, 1, y, INK);                           // the lines it hangs from
  rect(x + w - 21, 0, 1, y, INK);
  rect(x + 1, y + 1, w, h, W3);                         // shadow
  rect(x, y, w, h, INK);
  rect(x + 2, y + 2, w - 4, h - 4, FOAM);
  rect(x + 2, y + 2, w - 4, 2, AMBER);                  // a strip of amber tape along the top
  labelFont.setForegroundColor(INK);
  for (uint8_t l = 0; l < count; l++) {
    labelFont.drawUTF8((DISP_W - labelFont.getUTF8Width(lines[l])) / 2, y + 17 + 14 * l, lines[l]);
  }
  for (uint8_t i = 0; i < 2; i++) {                     // bubbles rising past it
    unsigned long t = (now + i * 450) % 900;
    rect(x + w + 4 - i * 2, y + h - t * h / 900, 2, 2, FOAM);
  }
}

const Theme THEME_AQUARIUM = {
  8, "aquarium", "水族箱", "小队在鱼缸里各游各的", 0x2BB3C0, 3, 4, 0,   // id key name tagline glow minFw perPage pageMs
  {
    W2, W3, AMBER, RED,                    // bg gap waitFlash errorFrame
    INK, FOAM, INK, W2, FOAM,              // plate tileBorder text tag more
    SAND, FOAM, PEBBLE,                    // pagerBg pagerOn pagerOff
    SURF, W3,                              // smoke sleepTarget: sleepers fade into the deep water
    INK, FOAM,                             // toastBg toastText
    {SURF, BLUE, WEED, AMBER, FOAM, RED, SLATE},
    {PINK, LIME, VIOLET, SKY, PEACH, LILAC},
    {AMBER, RED, FOAM, PINK, LIME, SKY},
  },
  SKINS, KNOT_LOGO,
  {ICON_GEAR_A, ICON_GEAR_B, ICON_KEYS_A, ICON_KEYS_B, ICON_ASK, ICON_CHECK, ICON_CROSS, ICON_ZZZ},
  {nullptr, nullptr, emptyTank, nullptr, page, transition, noPager, nullptr, notice, nullptr},
  // Squad's scene is 7200 ms already. Solo needs 18000, the least multiple of the 3600 ms seaweed and
  // bubbles and the 500 ms waiting hop; the empty tank closes in 3600.
  {{0, 0, 0}, {0, 18000, 0}, {0, 3600, 0}},
};
