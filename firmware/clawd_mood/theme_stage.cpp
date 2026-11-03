// Theme 5, 剧场: a pixel theatre. One session per page stands in the spotlight and acts out its
// state; the others sit in the front row with a state lamp each. Pages change with a curtain swish.
// Everything is a pure function of `now` and the session table, so no enter hook is needed.
// Every animation period divides 6000 or 7200 ms, so the previews loop.
#include "theme.h"

// 16 colours on a squad frame: keep it that way so a preview sheet stays at 4 bits per pixel.
#define BACK     RGB(20, 10, 0)      // all 565 channels even: dimColor(BACK) == BACK
#define CURT     RGB(142, 27, 36)
#define CURT_DK  RGB(90, 15, 22)
#define CURT_HI  RGB(184, 50, 60)
#define GOLD     RGB(232, 176, 75)
#define PLANK    RGB(107, 65, 40)
#define PLANK_DK RGB(78, 46, 28)
#define CREAM    RGB(255, 241, 201)
#define ORANGE   RGB(218, 119, 86)
#define BLUE     RGB(90, 124, 255)
#define TEAL     RGB(13, 200, 165)
#define GREEN    RGB(80, 210, 90)
#define RED      RGB(255, 51, 68)
#define AMBER    RGB(255, 214, 40)
#define DUSK     RGB(96, 112, 220)
#define LILAC    RGB(190, 160, 255)

// Stage geometry
#define CURTAIN_W  24          // each side curtain when open
#define HEM_Y      204         // curtains end here
#define BACK_Y     186         // floor starts
#define LIP_Y      208         // stage front edge with the footlights
#define HOUSE_Y    212         // the audience below the stage
#define SEAT_Y     228         // top of an occupied seat back
#define LEAD_U     5           // lead creature: 60 x 45 px
#define LEAD_X     90
#define LEAD_FEET  198
#define WING_FEET  190         // understudies stand further back

static const uint8_t ICON_GEAR_A[7]  = {0x08,0x5D,0x3E,0x77,0x3E,0x5D,0x08};
static const uint8_t ICON_GEAR_B[7]  = {0x22,0x1C,0x3E,0x36,0x3E,0x1C,0x22};
static const uint8_t ICON_KEYS_A[7]  = {0x00,0x7F,0x55,0x7F,0x6B,0x7F,0x00};
static const uint8_t ICON_KEYS_B[7]  = {0x00,0x7F,0x6B,0x7F,0x55,0x7F,0x00};
static const uint8_t ICON_ASK[7]     = {0x3E,0x63,0x03,0x0E,0x08,0x00,0x08};
static const uint8_t ICON_CHECK[7]   = {0x00,0x01,0x03,0x46,0x6C,0x38,0x10};
static const uint8_t ICON_CROSS[7]   = {0x63,0x77,0x3E,0x1C,0x3E,0x77,0x63};
static const uint8_t ICON_ZZZ[7]     = {0x7F,0x03,0x06,0x0C,0x18,0x30,0x7F};

// The cast, relit for a dark stage: black outlines turn cream, faces go dark where a face was light.
static const Skin SKINS[] = {
  {"CC", {".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB."},
   ORANGE, 0, 0, BACK, 0, 11, 2, 8, 2, 3, 7, 2, EYES_SQUARE},
  {"CX", {"..BBBBBBBB..",      // white robot, the knot as a hat
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          "..BBBBBBBB.."},
   CREAM, 0, CREAM, BACK, 0, 11, 3, 7, 2, 3, 7, 2, EYES_SQUARE, true},
  {"OC", {".BBBBBBBBBB.",      // hollow frame: the stage shows through
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BBBBBBBBBB."},
   CREAM, 0, BACK, CREAM, 0, 11, 3, 7, 2, 3, 7, 2, EYES_SQUARE},
  {"PI", {"BBBBBBBBB...",
          "BBBFFFFBB...",
          "BBBFFFFBB...",
          "BBBFFFFBB...",
          "BBBBBB...BBB",
          "BBBBBB...BBB",
          "BBB......BBB"},
   LILAC, 0, CREAM, BACK, -1, -1, 0, 9, 3, 4, -1, 1, EYES_SQUARE},
  {"DS", {"..........BB",
          "..BBBBB..BB.",
          ".BBBBBBB.BB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBB..",
          ".AAAAAAA....",
          "..AAAAAA...."},
   BLUE, CREAM, 0, CREAM, 0, -1, 2, 6, 2, 2, -1, 2, EYES_SQUARE},
  {"WB", {".B........B.",
          ".BB......BB.",
          ".BBBBBBBBBB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BBBBBBBBBB."},
   TEAL, 0, BACK, TEAL, 0, 11, 3, 7, 2, 3, 7, 3, EYES_SLASH},
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

// Ordered dither: a pixel is set where the 4x4 Bayer value is below level (0..16).
static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static void dither(int16_t x0, int16_t x1, int16_t y, uint8_t level, uint16_t c) {
  x0 += OX;
  x1 += OX;
  if (y < 0 || y >= DISP_H) return;
  if (x0 < 0) x0 = 0;
  if (x1 > DISP_W) x1 = DISP_W;
  if (x0 >= x1) return;
  uint16_t* row = canvas.getBuffer() + y * DISP_W;
  const uint8_t* b = BAYER[y & 3];
  for (uint8_t k = 0; k < 4; k++) {
    if (b[k] >= level) continue;
    for (int16_t x = x0 + ((k - x0) & 3); x < x1; x += 4) row[x] = c;
  }
}

static void rounded(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
  rect(x + 2, y, w - 4, 1, c);
  rect(x + 1, y + 1, w - 2, 1, c);
  rect(x, y + 2, w, h - 4, c);
  rect(x + 1, y + h - 2, w - 2, 1, c);
  rect(x + 2, y + h - 1, w - 4, 1, c);
}

static void text5x7(const char* s, int16_t x, int16_t y, uint8_t size, uint16_t c) {
  canvas.setTextSize(size);
  canvas.setTextColor(c);
  canvas.setCursor(x + OX, y);
  canvas.print(s);
  canvas.setTextSize(1);
}

// ── Scenery ─────────────────────────────────────────────────────

// Velvet folds, 12 px repeating from the curtain's free edge outwards so they travel with it
static const struct { uint8_t at, w; uint16_t c; } FOLD[] = {{0, 2, CURT_HI}, {2, 4, CURT}, {6, 3, CURT_DK}, {9, 3, CURT}};

// Left curtain covers [0, w), the right one [DISP_W - w, DISP_W); rows [0, HEM_Y)
static void drapes(int16_t w, bool tied) {
  for (int16_t base = 0; base < w; base += 12) {
    for (const auto& f : FOLD) {
      int16_t a = base + f.at, b = min<int16_t>(a + f.w, w);
      if (a >= w) break;
      rect(w - b, 0, b - a, HEM_Y, f.c);              // left: measured leftwards from its edge
      rect(DISP_W - w + a, 0, b - a, HEM_Y, f.c);     // right: rightwards
    }
  }
  rect(0, HEM_Y - 3, w, 1, GOLD);                     // hem braid
  rect(DISP_W - w, HEM_Y - 3, w, 1, GOLD);
  rect(0, HEM_Y - 2, w, 2, CURT_DK);
  rect(DISP_W - w, HEM_Y - 2, w, 2, CURT_DK);
  if (tied) {                                         // gold tie-backs, open curtains only
    rect(0, 148, w, 3, GOLD);
    rect(DISP_W - w, 148, w, 3, GOLD);
    rect(w - 4, 151, 3, 6, GOLD);
    rect(DISP_W - w + 1, 151, 3, 6, GOLD);
  }
}

// Scalloped pelmet with a gold fringe across the top
static void valance() {
  rect(0, 0, DISP_W, 10, CURT);
  rect(0, 2, DISP_W, 1, CURT_HI);
  rect(0, 8, DISP_W, 1, CURT_DK);
  static const uint8_t INSET[] = {0, 1, 3, 6};
  for (int16_t x = 0; x < DISP_W; x += 20) {
    for (uint8_t r = 0; r < 4; r++) {
      rect(x + INSET[r], 10 + r + 1, 20 - 2 * INSET[r], 1, GOLD);   // fringe peeks out under the scallop
      rect(x + INSET[r], 10 + r, 20 - 2 * INSET[r], 1, CURT);
    }
    rect(x + 9, 15, 2, 2, GOLD);                                    // tassel
  }
}

static void floorBoards(bool lights) {
  rect(0, BACK_Y - 1, DISP_W, 1, PLANK_DK);
  rect(0, BACK_Y, DISP_W, LIP_Y - BACK_Y, PLANK);
  static const uint8_t SEAM_OFF[] = {0, 22, 11};
  for (uint8_t r = 0; r < 3; r++) {
    int16_t y = BACK_Y + r * 7;
    rect(0, y + 6, DISP_W, 1, PLANK_DK);
    for (int16_t x = SEAM_OFF[r]; x < DISP_W; x += 44) rect(x, y, 1, 6, PLANK_DK);
  }
  rect(0, LIP_Y, DISP_W, HOUSE_Y - LIP_Y, PLANK_DK);
  rect(0, LIP_Y, DISP_W, 1, GOLD);
  for (int16_t x = 6; x < DISP_W; x += 16) rect(x, LIP_Y + 1, 4, 2, lights ? CREAM : PLANK);
}

// A light board above the stage. Bulbs chase around it, or all flash in an urgent state's colour.
static void marquee(const char* text, uint16_t textCol, uint16_t bulbCol, uint16_t chaseMs, bool flash,
                    unsigned long now) {
  const int16_t x = 44, y = 18, w = 152, h = 28;
  rect(60, 10, 1, y - 10, GOLD);                      // hangers
  rect(179, 10, 1, y - 10, GOLD);
  rect(x, y, w, h, GOLD);
  rect(x + 1, y + 1, w - 2, h - 2, CURT_DK);
  rect(x + 4, y + 4, w - 8, h - 8, GOLD);
  rect(x + 5, y + 5, w - 10, h - 10, BACK);
  // Bulbs on the frame, numbered clockwise so a chase runs around it
  uint8_t step = chaseMs ? (now / chaseMs) % 3 : 0;
  bool lit = flash ? (now / 300) % 2 : true;
  uint8_t i = 0;
  auto bulb = [&](int16_t bx, int16_t by) {
    bool on = chaseMs ? (i % 3 == step) : (flash ? lit : bulbCol != PLANK);
    rect(bx, by, 2, 2, on ? bulbCol : PLANK);
    i++;
  };
  for (int16_t bx = x + 3; bx < x + w - 3; bx += 6) bulb(bx, y + 1);
  for (int16_t by = y + 7; by < y + h - 3; by += 6) bulb(x + w - 3, by);
  for (int16_t bx = x + w - 5; bx > x + 1; bx -= 6) bulb(bx, y + h - 3);
  for (int16_t by = y + h - 9; by > y + 3; by -= 6) bulb(x + 1, by);
  char label[sizeof(Session::title)];
  int16_t lw = fitLabel(label, sizeof(label), text, w - 16);
  labelFont.setForegroundColor(textCol);
  labelFont.drawUTF8(x + (w - lw) / 2 + OX, y + 19, label);
}

// ── The lead and the act ────────────────────────────────────────

static const uint8_t ERROR_FLICKER[6] = {1, 1, 0, 1, 0, 1};   // per 100 ms frame: 600 ms

// The spotlight: a dithered cone from the light board to a pool on the boards
static void spotlight(int16_t cx, int16_t hwTop, int16_t hwBot, uint8_t level, uint16_t c) {
  const int16_t top = 47, bottom = BACK_Y - 1;
  for (int16_t y = top; y < bottom; y++) {
    int16_t hw = hwTop + (hwBot - hwTop) * (y - top) / (bottom - top);
    uint8_t core = y < top + 24 ? level + 2 : level;
    dither(cx - hw, cx - hw + 2, y, 8, c);
    dither(cx - hw + 2, cx + hw - 2, y, core, c);
    dither(cx + hw - 2, cx + hw, y, 8, c);
  }
  static const uint8_t POOL[] = {25, 41, 50, 57, 61, 63, 64, 63, 61, 57, 50, 41, 25};   // ellipse, /64
  for (uint8_t r = 0; r < sizeof(POOL); r++) {
    int16_t hw = (hwBot + 6) * POOL[r] / 64, y = LEAD_FEET - 7 + r;
    dither(cx - hw, cx + hw, y, level + 4, c);
  }
}

// Understudies: a session's subagents, small copies waiting in the wings behind the lead
static void understudies(const Session& s, unsigned long now) {
  static const int16_t WING_X[4] = {156, 60, 182, 34};
  uint8_t n = s.followerCount, shown = n <= 4 ? n : 3;
  for (uint8_t i = 0; i < shown; i++) {
    const Follower& f = s.followers[i];
    unsigned long age = now - f.bornMs;
    int16_t drop = age < 400 ? (400 - age) / 40 : 0;   // drops in from the fly loft
    drawCreature(s.src, s.color, f.mood, f.moodSinceMs, f.mood == MOOD_WAITING, WING_X[i] + OX,
                 WING_FEET - drop, 2, now + i * 110);
  }
  if (n > shown) {
    char more[5];
    snprintf(more, sizeof(more), "+%u", n - shown);
    text5x7(more, WING_X[3] + 4, WING_FEET - 12, 1, GOLD);
  }
}

static void thoughtBubble(unsigned long now) {
  rounded(146, 96, 42, 32, CREAM);
  rounded(152, 91, 14, 8, CREAM);
  rounded(168, 90, 14, 9, CREAM);
  rounded(140, 133, 6, 6, CREAM);
  rounded(134, 143, 4, 4, CREAM);
  glyph(157, 101, iconFor(MOOD_THINKING, now), 3, BLUE);
}

static void askBubble(unsigned long now) {
  int16_t dy = (now / 300) % 2 ? -2 : 0;
  rounded(46, 94 + dy, 40, 32, AMBER);
  rect(78, 126 + dy, 6, 2, AMBER);                  // tail towards the lead
  rect(82, 128 + dy, 5, 2, AMBER);
  rect(86, 130 + dy, 3, 2, AMBER);
  glyph(56, 99 + dy, ICON_ASK, 3, BACK);
}

static void keyboardProp(unsigned long now) {
  const int16_t x = 96, y = 189, w = 48;
  rect(x + 4, y + 6, 2, LEAD_FEET - y - 6, PLANK_DK);  // stand
  rect(x + w - 6, y + 6, 2, LEAD_FEET - y - 6, PLANK_DK);
  rect(x, y, w, 6, PLANK_DK);
  bool f = (now / 150) % 2;
  for (uint8_t k = 0; k < 7; k++) {
    rect(x + 2 + k * 6 + (f ? 0 : 0), y + 1, 4, 2, (k % 2) == f ? GOLD : CREAM);
    rect(x + 5 + k * 6 - 3, y + 3, 4, 2, (k % 2) != f ? GOLD : CREAM);
  }
  for (uint8_t i = 0; i < 3; i++) {                 // sparks off the keys
    uint8_t p = (now / 150 + i * 2) % 6;
    rect(x + 6 + i * 16 + p, y - 3 - p, 1, 1, i == 1 ? GOLD : CREAM);
  }
}

static void zzz(unsigned long now) {
  for (uint8_t i = 0; i < 3; i++) {               // each z drifts up over 2.4 s, staggered
    uint16_t t = (now + i * 800) % 2400;
    text5x7("z", 150 + i * 8 + t / 240, 138 - i * 10 - t / 60, i + 1, t < 1800 ? CREAM : DUSK);
  }
}

static void curtainCall(const Session& s, unsigned long now) {
  glyph(110, 100, ICON_CHECK, 3, CREAM);
  static const struct { int16_t x, y; uint8_t phase; } STAR[] = {{80, 116, 0}, {150, 104, 1}, {164, 126, 2}};
  for (const auto& st : STAR) {                   // twinkling gold stars
    bool big = (now / 200 + st.phase) % 3 == 0;
    rect(st.x - 2, st.y, 5, 1, GOLD);
    rect(st.x, st.y - 2, 1, 5, GOLD);
    if (big) { rect(st.x - 3, st.y, 7, 1, GOLD); rect(st.x, st.y - 3, 1, 7, GOLD); }
  }
  if (now - s.moodSinceMs < 2000) {               // confetti rain through the light
    for (uint8_t i = 0; i < 14; i++) {
      int16_t x = 66 + (i * 37 + (now / 50) * (i % 3 ? 1 : -1)) % 108;
      if (x < 66) x += 108;
      int16_t y = 50 + (i * 29 + now / 12) % 140;
      rect(x, y, 2, 2, T->pal.confetti[i % 6]);
    }
  }
}

// Backdrop, light, boards, the lead and its act. No curtains, pelmet or front row.
static void stageBody(const Session* s, unsigned long now) {
  rect(CURTAIN_W, 0, DISP_W - 2 * CURTAIN_W, BACK_Y, BACK);
  Mood m = s ? s->mood : MOOD_IDLE;
  if (m == MOOD_UNKNOWN) m = MOOD_IDLE;
  floorBoards(m != MOOD_SLEEPING);
  rect(0, HOUSE_Y, DISP_W, DISP_H - HOUSE_Y, BACK);
  if (!s) return;

  const int16_t cx = 120;
  switch (m) {
    case MOOD_THINKING: spotlight(cx, 14, 44, 4, CREAM); break;   // the light narrows
    case MOOD_WAITING:  spotlight(cx, 20, 56, 5, (now / 300) % 2 ? AMBER : CREAM); break;
    case MOOD_ERROR:    spotlight(cx, 20, 56, ERROR_FLICKER[(now / 100) % 6] ? 5 : 1, RED); break;
    case MOOD_SLEEPING: spotlight(cx, 20, 56, 2, DUSK); break;
    case MOOD_DONE:     spotlight(cx, 20, 56, 6, CREAM); break;
    default:            spotlight(cx, 20, 56, 4, CREAM); break;
  }
  rect(LEAD_X - 2, LEAD_FEET - 1, 64, 2, PLANK_DK);   // shadow at the feet
  understudies(*s, now);
  drawCreature(s->src, s->color, m, s->moodSinceMs, false, LEAD_X + OX, LEAD_FEET, LEAD_U, now);
  switch (m) {
    case MOOD_THINKING: thoughtBubble(now); break;
    case MOOD_WORKING:  keyboardProp(now); break;
    case MOOD_WAITING:  askBubble(now); break;
    case MOOD_DONE:     curtainCall(*s, now); break;
    case MOOD_ERROR:   // the light flickers, the cross stays
      glyph(110, 98, ICON_CROSS, 3, RED);
      break;
    case MOOD_SLEEPING: zzz(now); break;
    default: break;
  }
}

static void leadBoard(const Session* s, unsigned long now) {
  if (!s) {
    marquee("", CREAM, PLANK, 0, false, now);
    return;
  }
  switch (s->mood) {
    case MOOD_WAITING:  marquee(labelOf(*s), CREAM, AMBER, 0, true, now); break;
    case MOOD_ERROR:    marquee(labelOf(*s), CREAM, RED, 0, true, now); break;
    case MOOD_SLEEPING: marquee(labelOf(*s), CREAM, PLANK, 0, false, now); break;
    case MOOD_IDLE:     marquee(labelOf(*s), CREAM, GOLD, 400, false, now); break;
    default:            marquee(labelOf(*s), CREAM, GOLD, 200, false, now); break;
  }
}

// ── The front row ───────────────────────────────────────────────

static void seatBack(int16_t sx, int16_t pitch, int16_t top, bool marked) {
  int16_t x = sx + 1, w = pitch - 2;
  rect(x + 1, top, w - 2, 1, CURT);
  rect(x, top + 1, w, DISP_H - top - 1, CURT_DK);
  rect(x, top + 1, w, 1, CURT);
  if (marked) {
    rect(x + 1, top, w - 2, 1, GOLD);
    rect(x, top + 1, 1, DISP_H - top - 1, GOLD);
    rect(x + w - 1, top + 1, 1, DISP_H - top - 1, GOLD);
  }
}

static void lamp(const Session& s, int16_t cx, unsigned long now) {
  Mood m = s.mood == MOOD_UNKNOWN ? MOOD_IDLE : s.mood;
  const uint8_t* icon = iconFor(m, now);
  uint16_t col = T->pal.state[m];
  if (!icon) {
    rect(cx - 1, SEAT_Y + 4, 3, 3, col);
    return;
  }
  if (isAlerting(s) && (now / 300) % 2) {          // an urgent seat flashes: lit plate, dark icon
    rect(cx - 5, SEAT_Y + 2, 11, 10, col);
    glyph(cx - 3, SEAT_Y + 3, icon, 1, BACK);
  } else {
    glyph(cx - 3, SEAT_Y + 3, icon, 1, col);
  }
}

// One seat per session in arrival order, centred, with empty seats running off both edges.
// The lead's seat is empty, trimmed in gold and labelled with its agent.
static void frontRow(int8_t lead, unsigned long now) {
  uint8_t n = sessionCount;
  int16_t pitch = n <= 8 ? 30 : DISP_W / n;
  uint8_t u = pitch >= 28 ? 2 : 1;
  int16_t start = (DISP_W - n * pitch) / 2;
  for (int16_t k = -((start + pitch - 1) / pitch); start + k * pitch < DISP_W; k++) {
    int16_t sx = start + k * pitch, cx = sx + pitch / 2;
    if (k < 0 || k >= n) {
      seatBack(sx, pitch, SEAT_Y - 8, false);
      continue;
    }
    const Session& s = sessions[k];
    if (k == lead) {
      seatBack(sx, pitch, SEAT_Y - 8, true);
      rect(cx - 1, HOUSE_Y + 3, 3, 1, GOLD);         // points at the stage
      rect(cx - 2, HOUSE_Y + 4, 5, 1, GOLD);
      rect(cx - 3, HOUSE_Y + 5, 7, 1, GOLD);
      if (s.src[0]) text5x7(s.src, cx - (int16_t)strlen(s.src) * 3, SEAT_Y + 2, 1, GOLD);
      continue;
    }
    // Seated: the seat back hides the legs; a sleeper slumps lower
    int16_t feet = s.mood == MOOD_SLEEPING ? SEAT_Y : SEAT_Y + 2 * u;
    drawCreature(s.src, s.color, s.mood, s.moodSinceMs, false, sx + (pitch - 12 * u) / 2 + OX, feet, u, now);
    seatBack(sx, pitch, SEAT_Y, false);
    lamp(s, cx, now);
  }
}

// ── Hooks ───────────────────────────────────────────────────────

static void page(uint8_t first, uint8_t count, int16_t ox, int16_t, unsigned long now) {
  OX = ox;
  const Session* s = count ? &sessions[first] : nullptr;
  stageBody(s, now);
  drapes(CURTAIN_W, true);
  valance();
  leadBoard(s, now);
  frontRow(count ? first : -1, now);
  OX = 0;
}

// A curtain swish: the drapes close over the old lead and open on the new one.
static void transition(uint8_t from, uint8_t to, uint16_t progress, int16_t, unsigned long now) {
  OX = 0;
  bool second = progress >= TRANSITION_ONE / 2;
  uint8_t shown = second ? to : from;
  const Session* s = shown < sessionCount ? &sessions[shown] : nullptr;
  uint16_t closed = second ? (TRANSITION_ONE - progress) * 2 : progress * 2;   // 0..256
  stageBody(s, now);
  drapes(CURTAIN_W + (DISP_W / 2 - CURTAIN_W) * closed / TRANSITION_ONE, false);
  valance();
  leadBoard(s, now);
  frontRow(s ? shown : -1, now);
}

// Nobody connected: the curtains are closed, the board reads 今日休演 and its bulbs chase slowly.
static void closedHouse(unsigned long now) {
  OX = 0;
  floorBoards(false);
  rect(0, HOUSE_Y, DISP_W, DISP_H - HOUSE_Y, BACK);
  drapes(DISP_W / 2, false);
  // A notice on an easel in front of the curtain
  rect(92, 176, 2, LIP_Y - 176, PLANK_DK);
  rect(146, 176, 2, LIP_Y - 176, PLANK_DK);
  rect(80, 158, 80, 22, GOLD);
  rect(81, 159, 78, 20, PLANK);
  labelFont.setForegroundColor(CREAM);
  labelFont.drawUTF8(120 - labelFont.getUTF8Width("等待连接") / 2, 174, "等待连接");
  valance();
  marquee("今日休演", CREAM, GOLD, 500, false, now);
  frontRow(-1, now);
}

static int16_t noPager(uint8_t) { return 0; }

// The settings notice hangs from the light board on two ropes; long ones break at item gaps.
static void notice(const char* text, unsigned long now) {
  char lines[3][64];
  uint8_t count = 0;
  const int16_t maxW = 168;
  const char* p = text;
  lines[0][0] = 0;
  while (*p && count < 3) {
    const char* gap = strstr(p, "  ");
    size_t len = gap ? (size_t)(gap - p) : strlen(p);
    char item[64];
    snprintf(item, sizeof(item), "%.*s", (int)len, p);
    char joined[64];
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
  const int16_t w = 192, x = (DISP_W - w) / 2, h = 14 * count + 14, y = 62;
  rect(60, 46, 1, y - 46, GOLD);
  rect(179, 46, 1, y - 46, GOLD);
  rect(x, y, w, h, GOLD);
  rect(x + 1, y + 1, w - 2, h - 2, CURT_DK);
  rect(x + 3, y + 3, w - 6, h - 6, CURT);
  rect(x + 4, y + 4, w - 8, h - 8, CURT_DK);
  uint8_t step = (now / 200) % 3, i = 0;
  for (int16_t bx = x + 6; bx < x + w - 6; bx += 6, i++) {   // bulbs chase along the top edge
    rect(bx, y + 1, 2, 2, i % 3 == step ? CREAM : PLANK);
    rect(x + w - 8 - (bx - x - 6), y + h - 3, 2, 2, i % 3 == step ? CREAM : PLANK);
  }
  labelFont.setForegroundColor(CREAM);
  for (uint8_t l = 0; l < count; l++) {
    labelFont.drawUTF8((DISP_W - labelFont.getUTF8Width(lines[l])) / 2, y + 19 + 14 * l, lines[l]);
  }
}

const Theme THEME_STAGE = {
  5, "stage", "剧场", "聚光灯下只演一个主角", 0xE8B04B, 2, 1,
  // A page cycle of 5095 + 305 = 5400 ms on the host, so 4 pages take 21600 ms = 3 x 7200 ms, the
  // period of every animation in the squad scene. With 5000 the cycle is 5305 ms and never closes.
  5095,
  {
    BACK, BACK, AMBER, RED,                 // bg gap waitFlash errorFrame
    CURT_DK, GOLD, CREAM, GOLD, GOLD,       // plate tileBorder text tag more
    BACK, GOLD, PLANK_DK,                   // pagerBg pagerOn pagerOff
    CREAM, BACK,                            // smoke sleepTarget
    CURT_DK, CREAM,                         // toastBg toastText
    {GOLD, BLUE, GREEN, AMBER, CREAM, RED, DUSK},
    {GREEN, GOLD, LILAC, BLUE, CREAM, TEAL},
    {GOLD, CREAM, RED, BLUE, TEAL, GREEN},
  },
  SKINS, KNOT_LOGO,
  {ICON_GEAR_A, ICON_GEAR_B, ICON_KEYS_A, ICON_KEYS_B, ICON_ASK, ICON_CHECK, ICON_CROSS, ICON_ZZZ},
  {nullptr, nullptr, closedHouse, nullptr, page, transition, noPager, nullptr, notice, nullptr},
  // The squad scene's error session holds the screen for 10 s from the start; settle past it, then
  // one full round of the 4 pages. 20 fps keeps every frame of each page change.
  {{11000, 21600, 20}, {0, 0, 0}, {0, 0, 0}},
};
