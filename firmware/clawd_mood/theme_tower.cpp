// Theme 7, 深夜公寓: a brick apartment block at night. Every session is a lit window, two per floor and
// three floors per page, its room lit in the state's light with the agent acting inside and a brass
// nameplate under the sill. A pink neon sign on the roof counts the households; more than six pan the
// floors like an elevator, with a car riding a cable down the building's right edge. One session gets a
// close-up of one big window; nobody connected leaves every window dark and the sign blinks 空房.
// Everything is a pure function of `now` and the session table, so there is no enter hook.
// Animation periods divide 1200 ms (2400 only for thinking, which no 6000 ms preview shows), so the
// previews loop.
#include "theme.h"

// Palette. Sleepers fade halfway to BLUE, as if lit by the moon. The moonbeam is darker than any faded
// colour (a faded near-black is about RGB(75, 98, 151)), so a dark agent lying in it keeps its outline.
#define SKY     RGB(16, 16, 48)
#define BEAM    RGB(32, 48, 96)
#define WINE    RGB(120, 24, 40)    // the close-up's state word: 4.8:1 on the brass plate
#define BRICK   RGB(58, 42, 63)
#define MORTAR  RGB(40, 28, 44)
#define ROOF    RGB(90, 69, 96)
#define NEON    RGB(255, 95, 162)
#define BRASS   RGB(217, 164, 65)
#define CREAM   RGB(255, 244, 220)
#define GREY    RGB(136, 130, 134)
#define LAMP    RGB(255, 200, 107)
#define IDLE    RGB(122, 90, 46)
#define BLUE    RGB(111, 168, 255)
#define BLUE_DK RGB(63, 99, 200)
#define RED     RGB(255, 74, 74)
#define RED_DK  RGB(168, 40, 58)
#define AMBER   RGB(255, 176, 0)
#define GREEN   RGB(56, 199, 127)
#define TEAL    RGB(30, 168, 154)
#define ORANGE  RGB(240, 122, 58)
#define PURPLE  RGB(150, 80, 220)

// Building geometry
#define FAC_X0   12            // facade, the sky shows on both sides
#define FAC_X1   228
#define ROOF_Y   24            // parapet
#define BAND_Y   30            // floors start here; the header above never moves
#define FLOOR_H  70
#define BAND_H   (3 * FLOOR_H)
#define WIN_W    90            // squad window frame
#define WIN_H    50
#define WIN_TOP  3             // frame top inside its floor
#define PLATE_Y  56            // nameplate top inside its floor
#define PLATE_H  13
#define CABLE_X  218           // the elevator's cable, right of the windows
static const int16_t WIN_X[2] = {18, 118};

static const uint8_t ICON_GEAR_A[7]  = {0x08,0x5D,0x3E,0x77,0x3E,0x5D,0x08};
static const uint8_t ICON_GEAR_B[7]  = {0x22,0x1C,0x3E,0x36,0x3E,0x1C,0x22};
static const uint8_t ICON_KEYS_A[7]  = {0x00,0x7F,0x55,0x7F,0x6B,0x7F,0x00};
static const uint8_t ICON_KEYS_B[7]  = {0x00,0x7F,0x6B,0x7F,0x55,0x7F,0x00};
static const uint8_t ICON_ASK[7]     = {0x3E,0x63,0x03,0x0E,0x08,0x00,0x08};
static const uint8_t ICON_CHECK[7]   = {0x00,0x01,0x03,0x46,0x6C,0x38,0x10};
static const uint8_t ICON_CROSS[7]   = {0x63,0x77,0x3E,0x1C,0x3E,0x77,0x63};
static const uint8_t ICON_ZZZ[7]     = {0x7F,0x03,0x06,0x0C,0x18,0x30,0x7F};

// The cast in lit rooms: dark outlines read on every room colour, and dim to the night sky asleep.
static const Skin SKINS[] = {
  {"CC", {".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB."},
   ORANGE, 0, 0, MORTAR, 0, 11, 2, 8, 2, 3, 7, 2, EYES_SQUARE},
  {"CX", {"..BBBBBBBB..",      // white buddy in a dark outline, the knot as a hat
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          "..BBBBBBBB.."},
   MORTAR, 0, CREAM, MORTAR, 0, 11, 3, 7, 2, 3, 7, 2, EYES_SQUARE, true},
  {"OC", {".BBBBBBBBBB.",      // hollow black frame
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BBBBBBBBBB."},
   MORTAR, 0, CREAM, MORTAR, 0, 11, 3, 7, 2, 3, 7, 2, EYES_SQUARE},
  {"PI", {"BBBBBBBBB...",      // pixel P whose counter is a single eye
          "BBBFFFFBB...",
          "BBBFFFFBB...",
          "BBBFFFFBB...",
          "BBBBBB...BBB",
          "BBBBBB...BBB",
          "BBB......BBB"},
   MORTAR, 0, CREAM, MORTAR, -1, -1, 0, 9, 3, 4, -1, 1, EYES_SQUARE},
  {"DS", {"..........BB",      // whale in profile, tail up
          "..BBBBB..BB.",
          ".BBBBBBB.BB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBB..",
          ".AAAAAAA....",
          "..AAAAAA...."},
   BLUE_DK, CREAM, 0, CREAM, 0, -1, 2, 6, 2, 2, -1, 2, EYES_SQUARE},
  {"WB", {".B........B.",      // cat with a dark mask
          ".BB......BB.",
          ".BBBBBBBBBB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BBBBBBBBBB."},
   TEAL, 0, SKY, CREAM, 0, 11, 3, 7, 2, 3, 7, 3, EYES_SLASH},
  {"CU", {"....AAAA....",      // isometric cube: cream top, grey left, dark right
          "..AAAAAAAA..",
          ".BAAAAAAAAF.",
          ".BBBAAAAFFF.",
          ".BBBBBFFFFF.",
          ".BBBBBFFFFF.",
          "...BBBFFF..."},
   GREY, CREAM, ROOF, MORTAR, 0, 11, 3, 7, 2, 3, 7, 4, EYES_SQUARE},
};
static_assert(sizeof(SKINS) / sizeof(SKINS[0]) == SKIN_COUNT, "every theme carries all agents");

// ── Primitives ──────────────────────────────────────────────────
// OX: x origin of the page being drawn. This theme draws its own page change at 0; the offset is only
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

static void text5x7(const char* s, int16_t x, int16_t y, uint8_t size, uint16_t c) {
  canvas.setTextSize(size);
  canvas.setTextColor(c);
  canvas.setCursor(x + OX, y);
  canvas.print(s);
  canvas.setTextSize(1);
}

static void rounded(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t k, uint16_t c) {
  rect(x + k, y, w - 2 * k, h, c);
  rect(x, y + k, w, h - 2 * k, c);
}

// 0 → 1200 → 0 over period ms, scaled to span
static int16_t tri(unsigned long now, uint16_t period, int16_t span) {
  int32_t t = now % period, half = period / 2;
  return (t < half ? t : period - t) * span / half;
}

static Mood shown(const Session& s) { return s.mood == MOOD_UNKNOWN ? MOOD_IDLE : s.mood; }

static const uint8_t ERROR_FLICKER[6] = {1, 1, 0, 1, 0, 1};   // per 100 ms frame: 600 ms

// ── A room ──────────────────────────────────────────────────────
// A room is drawn in glass (gx, gy, gw, gh) with prop unit k and creature unit cu: k 1 / cu 2 in a
// squad window, k 2 / cu 6 in the close-up. The scene is 12cu + 32k wide; roommates (the session's
// subagents) stand on the floor to its right.

struct Light { uint16_t wall, floor, bar; };

static Light lightFor(const Session& s, unsigned long now) {
  bool alert = isAlerting(s);
  switch (shown(s)) {
    case MOOD_THINKING: return {BLUE, BLUE_DK, BLUE_DK};
    case MOOD_WORKING:  return {LAMP, BRASS, BRASS};
    case MOOD_WAITING:  return {alert && (now / 300) % 2 ? LAMP : AMBER, BRASS, BRASS};
    case MOOD_DONE:     return {GREEN, TEAL, TEAL};
    case MOOD_ERROR:    return {alert && !ERROR_FLICKER[(now / 100) % 6] ? RED_DK : RED, RED_DK, RED_DK};
    case MOOD_SLEEPING: return {SKY, MORTAR, ROOF};
    default:            return {IDLE, MORTAR, MORTAR};
  }
}

static void floorLamp(int16_t x, int16_t floorY, uint8_t k) {
  rect(x, floorY - 20 * k, 6 * k, 4 * k, BRASS);          // shade
  rect(x + k, floorY - 16 * k, 4 * k, k, LAMP);           // a dim glow under it
  rect(x + 3 * k - (k + 1) / 2, floorY - 16 * k, k, 16 * k, MORTAR);
  rect(x + k, floorY - k, 4 * k, k, MORTAR);
}

static void teaTable(int16_t x, int16_t floorY, uint8_t k, unsigned long now) {
  rect(x, floorY - 7 * k, 10 * k, 2 * k, MORTAR);
  rect(x + 4 * k, floorY - 5 * k, 2 * k, 5 * k, MORTAR);
  rect(x + 3 * k, floorY - 10 * k, 3 * k, 3 * k, CREAM);  // cup
  rect(x + 6 * k, floorY - 9 * k, k, k, CREAM);           // handle
  for (uint8_t i = 0; i < 2; i++) {                        // steam curls up, 1200 ms
    uint8_t p = (now / 300 + i * 2) % 4;
    rect(x + 4 * k + ((p + i) % 2) * k, floorY - 12 * k - p * 2 * k, k, k, CREAM);
  }
}

static void desk(int16_t x, int16_t floorY, uint8_t k, unsigned long now) {
  rect(x, floorY - 9 * k, 20 * k, 2 * k, MORTAR);
  rect(x + k, floorY - 7 * k, k, 7 * k, MORTAR);
  rect(x + 18 * k, floorY - 7 * k, k, 7 * k, MORTAR);
  // Monitor: its glow is the brightest thing in the room
  int16_t mx = x + 3 * k, my = floorY - 21 * k;
  rect(mx, my, 14 * k, 10 * k, MORTAR);
  rect(mx + k, my + k, 12 * k, 8 * k, BLUE);
  rect(mx + 6 * k, my + 10 * k, 2 * k, k, MORTAR);
  static const uint8_t LINES[4][3] = {{8, 5, 9}, {5, 9, 6}, {9, 6, 4}, {6, 4, 8}};   // code scrolling up
  const uint8_t* l = LINES[(now / 300) % 4];
  for (uint8_t i = 0; i < 3; i++) rect(mx + 2 * k, my + (2 + 2 * i) * k, l[i] * k, k, CREAM);
}

static void pendant(int16_t cx, int16_t gy, int16_t bottom, uint8_t k) {
  int16_t top = max<int16_t>(bottom - 3 * k, gy + 2 * k);
  rect(cx - (k + 1) / 2, gy, k, top - gy, MORTAR);
  rect(cx - 4 * k, top, 9 * k, 3 * k, BRASS);
  rect(cx - k, top + 3 * k, 3 * k, k, CREAM);
}

static void thoughtBubble(int16_t x, int16_t y, uint8_t k, unsigned long now) {
  rounded(x, y, 17 * k, 13 * k, k, CREAM);
  glyph(x + 5 * k, y + 3 * k, iconFor(MOOD_THINKING, now), k, BLUE_DK);
  rect(x + k, y + 14 * k, 2 * k, 2 * k, CREAM);
  rect(x - k, y + 17 * k, k, k, CREAM);
}

static void askCard(int16_t x, int16_t y, uint8_t k) {
  rect(x, y, 18 * k, 20 * k, MORTAR);
  rect(x + k, y + k, 16 * k, 18 * k, CREAM);
  glyph(x + 2 * k, y + 3 * k, ICON_ASK, 2 * k, RED);
}

static void partyLights(int16_t gx, int16_t gy, int16_t gw, uint8_t k, unsigned long now) {
  rect(gx, gy + 2 * k, gw, k, MORTAR);
  uint8_t step = (now / 200) % 6, i = 0;
  for (int16_t x = gx + 2 * k; x < gx + gw - 2 * k; x += 6 * k, i++) {
    int16_t sag = (i % 2) * k;
    rect(x, gy + 3 * k + sag, 2 * k, 2 * k, T->pal.confetti[(i + step) % 6]);
  }
}

static void confettiRain(int16_t gx, int16_t gy, int16_t gw, int16_t floorY, uint8_t k, unsigned long now) {
  int16_t h = floorY - gy - 6 * k;
  uint8_t n = gw / (8 * k);
  for (uint8_t i = 0; i < n; i++) {
    int16_t x = gx + (i * 8 * k + ((now / 200 + i) % 3) * k) % gw;
    int16_t y = gy + 6 * k + (i * 37 * k + now * k / 25) % h;
    rect(x, y, k + (i % 2), k + (i % 2), T->pal.confetti[i % 6]);
  }
}

// A slanted band of moonlight from the top of the glass down to (landX, gy + gh), w wide
static void moonbeam(int16_t landX, int16_t w, int16_t gx, int16_t gy, int16_t gw, int16_t gh, uint8_t k) {
  for (int16_t y = 0; y < gh; y += k) {
    int16_t x = landX - (gh - y) / 2, x0 = max<int16_t>(x, gx), x1 = min<int16_t>(x + w, gx + gw);
    if (x1 > x0) rect(x0, gy + y, x1 - x0, k, BEAM);
  }
}

static void bed(int16_t x, int16_t w, int16_t floorY, uint8_t k) {
  rect(x, floorY - 9 * k, 2 * k, 9 * k, ROOF);             // headboard
  rect(x, floorY - 4 * k, w, 2 * k, ROOF);                 // frame
  rect(x + w - k, floorY - 2 * k, k, 2 * k, ROOF);
  rect(x + 2 * k, floorY - 6 * k, w - 2 * k, 2 * k, GREY); // mattress
}

static void zzz(int16_t x, int16_t y, uint8_t k, unsigned long now) {
  for (uint8_t i = 0; i < 3; i++) {
    uint16_t t = (now + i * 400) % 1200;
    text5x7("z", x + i * 4 * k + t * 3 * k / 1200, y - i * 5 * k - t * 5 * k / 1200, k, t < 900 ? CREAM : GREY);
  }
}

// Subagents as roommates, right-aligned on the floor; the nameplate carries the count
static void roommates(const Session& s, int16_t right, uint8_t count, int16_t floorY, uint8_t ru, bool alert,
                      unsigned long now) {
  const int16_t pitch = 12 * ru + 1;
  // Pick count of them, waiting ones first, kept in arrival order
  uint8_t pick[MAX_FOLLOWERS], n = 0, waiting = 0;
  for (uint8_t i = 0; i < s.followerCount; i++) waiting += s.followers[i].mood == MOOD_WAITING;
  uint8_t others = count > waiting ? count - waiting : 0;
  for (uint8_t i = 0; i < s.followerCount && n < count; i++) {
    bool w = s.followers[i].mood == MOOD_WAITING;
    if (w || others) { pick[n++] = i; if (!w) others--; }
  }
  for (uint8_t i = 0; i < n; i++) {
    const Follower& f = s.followers[pick[i]];
    unsigned long age = now - f.bornMs;
    int16_t drop = age < 400 ? (400 - age) / 40 * ru : 0;
    int16_t x = right - (n - i) * pitch + 1;
    drawCreature(s.src, s.color, f.mood, f.moodSinceMs, false, x + OX, floorY - drop, ru, now + i * 110);
    if (f.mood == MOOD_WAITING && (!alert || (now / 300) % 2)) {   // a tiny card of its own
      int16_t cy = floorY - 9 * ru - 12 * ru;
      rect(x + 6 * ru - 5 * ru, cy, 10 * ru, 10 * ru, MORTAR);
      rect(x + 6 * ru - 4 * ru, cy + ru, 8 * ru, 8 * ru, CREAM);
      glyph(x + 6 * ru - 3 * ru, cy + 2 * ru, ICON_ASK, ru, RED);
    }
  }
}

static void room(const Session& s, int16_t gx, int16_t gy, int16_t gw, int16_t gh, uint8_t k, uint8_t cu,
                 unsigned long now) {
  Mood m = shown(s);
  Light L = lightFor(s, now);
  const int16_t floorY = gy + gh - 8 * k, cw = 12 * cu, ch = 9 * cu, sceneW = cw + 32 * k;
  rect(gx, gy, gw, floorY - gy, L.wall);
  rect(gx, floorY, gw, gy + gh - floorY, L.floor);
  rect(gx + gw / 2 - k, gy, 2 * k, floorY - gy, L.bar);   // mullions, behind the room
  rect(gx, gy + gh * 3 / 10, gw, k, L.bar);
  if (k > 1) {                                            // the close-up has room for a painting
    int16_t px = gx + gw - 27 * k, py = gy + 10 * k;
    rect(px, py, 22 * k, 16 * k, MORTAR);
    rect(px + k, py + k, 20 * k, 14 * k, SKY);
    rect(px + 13 * k, py + 3 * k, 3 * k, 3 * k, CREAM);
    rect(px + k, py + 10 * k, 8 * k, 5 * k, ROOF);
    rect(px + 9 * k, py + 12 * k, 12 * k, 3 * k, ROOF);
    rect(px + 3 * k, py + 8 * k, 4 * k, 2 * k, ROOF);
  }

  // Roommates take the right of the floor; without any the scene is centred
  const uint8_t ru = k > 1 ? 2 : 1;
  const int16_t pitch = 12 * ru + 1, zone = gw - sceneW - 2;
  uint8_t mates = min<int>(s.followerCount, zone >= 0 ? (zone + 1) / pitch : 0);
  int16_t sx = s.followerCount ? gx + 1 : gx + (gw - sceneW) / 2;
  bool alert = isAlerting(s);
  const Skin& skin = skinFor(s.src);
  int16_t hat = skin.knotHat ? 5 * cu : 0, headTop = floorY - ch;

  switch (m) {
    case MOOD_THINKING: {
      int16_t cx = sx + 2 * k + tri(now, 2400, 8 * k);    // paces the room
      drawCreature(s.src, s.color, m, s.moodSinceMs, false, cx + OX, floorY, cu, now);
      thoughtBubble(sx + sceneW - 18 * k, max<int16_t>(gy + 2 * k, headTop - 16 * k), k, now);
      break;
    }
    case MOOD_WORKING: {
      desk(sx + k, floorY, k, now);
      int16_t cx = sx + 23 * k;
      pendant(cx + cw / 2, gy, headTop - hat - 2 * k, k);
      drawCreature(s.src, s.color, m, s.moodSinceMs, false, cx + OX, floorY, cu, now);
      break;
    }
    case MOOD_WAITING: {                                    // up against the glass with a card
      uint8_t wu = cu + k;                                 // the scene stays 12cu + 32k wide
      int16_t feet = gy + gh + (k + 1), dy = -((now / 250) % 2) * max<int16_t>(wu, 2);
      drawCreature(s.src, s.color, m, s.moodSinceMs, false, sx + OX, feet, wu, now);
      askCard(sx + 12 * wu + 2 * k, feet - 9 * wu - 4 * k + dy, k);
      break;
    }
    case MOOD_DONE:
      partyLights(gx, gy, gw, k, now);
      confettiRain(gx, gy, gw, floorY, k, now);
      drawCreature(s.src, s.color, m, s.moodSinceMs, false, sx + (sceneW - cw) / 2 + OX, floorY, cu, now);
      break;
    case MOOD_ERROR: {
      int16_t cx = sx + (sceneW - cw) / 2;
      if (k > 1) {                                          // the close-up fills with smoke from the head up
        for (uint8_t i = 0; i < 4; i++) {
          uint16_t t = (now + i * 300) % 1200;
          int16_t sz = (3 + t * 6 / 1200) * k, rise = headTop - gy - 12 * k;
          int16_t px = cx + cw - 2 * cu - t * 24 * k / 1200, py = headTop - 2 * k - t * rise / 1200 - sz;
          rounded(px, py, sz + k, sz, k, t < 900 ? GREY : ROOF);
        }
      }
      drawCreature(s.src, s.color, m, s.moodSinceMs, false, cx + OX, floorY, cu, now);
      break;
    }
    case MOOD_SLEEPING: {
      int16_t bx = sx + 4 * k, bw = cw + 12 * k, feet = floorY - 6 * k;
      moonbeam(bx + 2 * k, bw, gx, gy, gw, floorY - gy, k);
      bed(bx, bw, floorY, k);
      int16_t cx = bx + 4 * k;
      drawCreature(s.src, s.color, m, s.moodSinceMs, false, cx + OX, feet, cu, now);
      rect(cx + 7 * cu, feet - cu, bx + bw - cx - 7 * cu, cu, ROOF);   // blanket over the feet
      rect(cx + 7 * cu, feet - cu, bx + bw - cx - 7 * cu, k, GREY);
      zzz(cx + cw - 2 * k, feet - 4 * cu - 9 * k, k, now);
      break;
    }
    default: {
      floorLamp(sx + 2 * k, floorY, k);
      int16_t cx = sx + 12 * k;
      drawCreature(s.src, s.color, m, s.moodSinceMs, false, cx + OX, floorY, cu, now);
      teaTable(cx + cw + 3 * k, floorY, k, now);
      break;
    }
  }
  roommates(s, gx + gw - 1, mates, floorY, ru, alert, now);
}

// Window frame, drawn after the room so anything leaning on the glass tucks under it
static void frame(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t b, const Session* s, unsigned long now) {
  uint16_t c = MORTAR, edge = ROOF;
  if (s && shown(*s) == MOOD_WAITING) {                    // gold frame, flashing when it matters
    c = BRASS;
    edge = isAlerting(*s) && (now / 300) % 2 ? CREAM : AMBER;
  }
  rect(x, y, w, b, c);
  rect(x, y + h - b, w, b, c);
  rect(x, y, b, h, c);
  rect(x + w - b, y, b, h, c);
  rect(x + b - 1, y + b - 1, w - 2 * b + 2, 1, edge);      // inner lip
  rect(x + b - 1, y + b, 1, h - 2 * b, edge);
  rect(x - 2, y + h, w + 4, 2, ROOF);                      // sill
}

// Puffs leaving an error window from its right edge (x), rising through the brick: the left window's
// through the pier, the right window's past the facade's edge, so two errors on a floor don't merge.
static void smokeOut(int16_t x, int16_t y, unsigned long now) {
  for (uint8_t i = 0; i < 3; i++) {
    uint16_t t = (now + i * 400) % 1200;
    int16_t sz = 4 + t * 3 / 1200;
    rounded(x + t * 3 / 1200, y - t * 30 / 1200 - sz, sz + 1, sz, 1, t < 800 ? GREY : ROOF);
  }
}

// ── Nameplate ───────────────────────────────────────────────────
static const char* const STATE_WORD[7] = {"空闲", "思考中", "工作中", "等你批准", "完成", "出错", "睡着了"};

static void plate(const Session* s, int16_t x, int16_t y, int16_t w, int16_t h, bool word, unsigned long now) {
  if (!s) {
    rect(x, y, w, h, ROOF);
    rect(x, y + h, w, 1, MORTAR);
    return;
  }
  bool flash = shown(*s) == MOOD_WAITING && isAlerting(*s) && (now / 300) % 2;
  rect(x, y, w, h, flash ? CREAM : BRASS);
  rect(x, y + h, w, 1, MORTAR);
  int16_t left = x + 3, right = x + w - 3, ty = y + (h - 7) / 2;
  if (s->src[0]) {                                          // agent tag on a dark chip
    int16_t tw = strlen(s->src) * 6 - 1;
    rect(left - 1, ty - 1, tw + 4, 9, MORTAR);
    text5x7(s->src, left + 1, ty, 1, flash ? CREAM : BRASS);
    left += tw + 6;
  }
  if (s->followerCount) {
    char more[5];
    snprintf(more, sizeof(more), "+%u", s->followerCount);
    int16_t mw = strlen(more) * 6 - 1;
    text5x7(more, right - mw, ty, 1, MORTAR);
    right -= mw + 4;
  }
  if (word) {
    const char* wtxt = STATE_WORD[shown(*s)];
    int16_t ww = labelFont.getUTF8Width(wtxt);
    labelFont.setForegroundColor(WINE);
    labelFont.drawUTF8(right - ww + OX, y + h - 3, wtxt);
    right -= ww + 6;
  }
  drawLabel(labelOf(*s), left + OX, y + h - 3, right - left, MORTAR);
}

// ── The building ────────────────────────────────────────────────

static void brick(int16_t x0, int16_t x1, int16_t y0, int16_t y1, int16_t origin) {
  if (y0 < BAND_Y) y0 = BAND_Y;
  if (y1 > DISP_H) y1 = DISP_H;
  if (y0 >= y1) return;
  rect(x0, y0, x1 - x0, y1 - y0, BRICK);
  // Courses of 6 rows from origin: a mortar line, then joints every 16 px, staggered
  int16_t j = (y0 - origin) / 6 - (y0 < origin);
  for (int16_t cy = origin + j * 6; cy < y1; cy += 6, j++) {
    if (cy + 5 >= y0 && cy + 5 < y1) rect(x0, cy + 5, x1 - x0, 1, MORTAR);
    int16_t top = max<int16_t>(cy, y0), bot = min<int16_t>(cy + 5, y1);
    if (top >= bot) continue;
    for (int16_t x = x0 + ((j & 1) ? 8 : 0); x < x1; x += 16) rect(x, top, 1, bot - top, MORTAR);
  }
}

static void vacantGlass(int16_t gx, int16_t gy, int16_t gw, int16_t gh, uint8_t k) {
  rect(gx, gy, gw, gh, SKY);
  rect(gx + gw / 2 - k, gy, 2 * k, gh, MORTAR);
  rect(gx, gy + gh * 3 / 10, gw, k, MORTAR);
  for (int16_t i = 0; i * 4 * k < gh * 2 / 3; i++) {       // a streak of reflected sky
    rect(gx + 4 * k + i * 2 * k, gy + 2 * k + i * 4 * k, k, 4 * k, ROOF);
    if (i < 4) rect(gx + 9 * k + i * 2 * k, gy + 2 * k + i * 4 * k, k, 4 * k, ROOF);
  }
}

// One floor band of a page: three floors from y0, slot j holds sessions[first + j] when j < count
static void floors(uint8_t first, uint8_t count, int16_t y0, unsigned long now) {
  brick(FAC_X0, FAC_X1, y0, y0 + BAND_H, y0);
  for (uint8_t j = 0; j < 6; j++) {
    int16_t fy = y0 + (j / 2) * FLOOR_H;
    if (fy + FLOOR_H <= BAND_Y || fy >= DISP_H) continue;
    const Session* s = j < count ? &sessions[first + j] : nullptr;
    int16_t wx = WIN_X[j % 2], wy = fy + WIN_TOP;
    int16_t gx = wx + 3, gy = wy + 3, gw = WIN_W - 6, gh = WIN_H - 6;
    if (s) room(*s, gx, gy, gw, gh, 1, 2, now);
    else vacantGlass(gx, gy, gw, gh, 1);
    frame(wx, wy, WIN_W, WIN_H, 3, s, now);
    plate(s, wx + 2, fy + PLATE_Y, WIN_W - 4, PLATE_H, false, now);
    if (s && shown(*s) == MOOD_ERROR) smokeOut(wx + WIN_W, wy + 22, now);
  }
}

static const struct { uint8_t x, y, phase; } STARS[] = {
  {6, 8, 0}, {88, 6, 3}, {110, 17, 1}, {138, 4, 4}, {160, 13, 2}, {196, 20, 5}, {232, 7, 1},
  {4, 52, 2}, {234, 74, 4}, {6, 118, 5}, {235, 148, 0}, {4, 186, 3}, {233, 214, 2}, {7, 232, 4},
};

// The stars in rows [y0, y1)
static void sky(int16_t y0, int16_t y1, unsigned long now) {
  for (const auto& st : STARS) {                         // twinkle, 1200 ms
    if (st.y < y0 || st.y >= y1) continue;
    uint8_t p = (now / 200 + st.phase) % 6;
    if (p == 0) {
      rect(st.x - 1, st.y, 3, 1, CREAM);
      rect(st.x, st.y - 1, 1, 3, CREAM);
    } else {
      rect(st.x, st.y, 1, 1, p == 3 ? ROOF : CREAM);
    }
  }
}

// The sky either side of the facade, below the header; the facade covers the rest
static void sideSky(unsigned long now) {
  rect(0, BAND_Y, FAC_X0, DISP_H - BAND_Y, SKY);
  rect(FAC_X1, BAND_Y, DISP_W - FAC_X1, DISP_H - BAND_Y, SKY);
  sky(BAND_Y, DISP_H, now);
}

// The sign on the roof: neon outline and letters on a dark board. lit false: the tubes are off.
// digits, when given, come first in the 5x7 font at double size, like tube numerals
static void sign(const char* digits, const char* text, bool lit) {
  int16_t dw = digits ? strlen(digits) * 12 - 2 : 0, tw = labelFont.getUTF8Width(text);
  int16_t w = dw + (digits ? 4 : 0) + tw + 14, x = 22, y = 3, h = 19;
  uint16_t c = lit ? NEON : ROOF;
  rect(x + 4, y + h, 1, ROOF_Y - y - h, ROOF);            // posts
  rect(x + w - 5, y + h, 1, ROOF_Y - y - h, ROOF);
  rect(x, y, w, h, MORTAR);
  rect(x + 1, y + 1, w - 2, 1, c);
  rect(x + 1, y + h - 2, w - 2, 1, c);
  rect(x + 1, y + 2, 1, h - 4, c);
  rect(x + w - 2, y + 2, 1, h - 4, c);
  if (digits) text5x7(digits, x + 7, y + 3, 2, c);
  labelFont.setForegroundColor(c);
  labelFont.drawUTF8(x + 7 + (digits ? dw + 4 : 0) + OX, y + 15, text);
}

static void header(const char* digits, const char* signText, bool signLit, unsigned long now) {
  rect(0, 0, DISP_W, BAND_Y, SKY);
  sky(0, BAND_Y, now);
  // Moon, top right
  static const uint8_t MOON[12] = {4, 8, 10, 10, 12, 12, 12, 12, 10, 10, 8, 4};
  for (uint8_t r = 0; r < 12; r++) rect(210 - MOON[r] / 2, 3 + r, MOON[r], 1, CREAM);
  rect(206, 6, 2, 2, LAMP);
  rect(211, 10, 3, 2, LAMP);
  rect(207, 12, 2, 1, LAMP);
  // Antenna with a beacon, 1200 ms
  rect(182, 9, 1, ROOF_Y - 9, ROOF);
  rect(178, 13, 9, 1, ROOF);
  rect(180, 17, 5, 1, ROOF);
  rect(181, 7, 3, 2, (now / 600) % 2 ? RED : RED_DK);
  // Parapet
  rect(FAC_X0 - 4, ROOF_Y, FAC_X1 - FAC_X0 + 8, 4, ROOF);
  rect(FAC_X0 - 4, ROOF_Y + 4, FAC_X1 - FAC_X0 + 8, 2, MORTAR);
  sign(digits, signText, signLit);
}

// The elevator: a cable down the right edge, a stop per page, the car at the page shown
static void elevator(uint8_t pages, int16_t carY, unsigned long now) {
  if (pages < 2) return;
  const int16_t pitch = 12, top = BAND_Y + (BAND_H - (pages - 1) * pitch) / 2;
  rect(CABLE_X, BAND_Y, 1, BAND_H, MORTAR);
  for (uint8_t p = 0; p < pages; p++) {
    uint16_t c = ROOF;
    for (uint8_t i = p * T->perPage; i < sessionCount && i < (p + 1) * T->perPage; i++) {
      if (isAlerting(sessions[i]) && (now / 250) % 2) c = stateColor(sessions[i].mood);
    }
    rect(CABLE_X - 1, top + p * pitch - 1, 3, 3, c);
  }
  int16_t y = top + carY * pitch / 256 - 3;
  rect(CABLE_X - 3, y, 7, 7, NEON);
  rect(CABLE_X - 2, y + 1, 5, 5, MORTAR);
  rect(CABLE_X - 1, y + 2, 3, 3, LAMP);
}

static void countSign(char* out, size_t n) { snprintf(out, n, "%u", sessionCount); }

// ── The close-up ────────────────────────────────────────────────

static void closeUp(const Session& s, unsigned long now) {
  const int16_t x = 14, y = 34, w = 212, h = 168, b = 4;
  sideSky(now);
  brick(FAC_X0 - 4, FAC_X1 + 4, BAND_Y, DISP_H, BAND_Y);
  room(s, x + b, y + b, w - 2 * b, h - 2 * b, 2, 6, now);
  frame(x, y, w, h, b, &s, now);
  plate(&s, 24, 210, 192, 17, true, now);
  char text[12];
  countSign(text, sizeof(text));
  header(text, "户", true, now);
}

// ── Hooks ───────────────────────────────────────────────────────

static void page(uint8_t first, uint8_t count, int16_t ox, int16_t, unsigned long now) {
  OX = ox;
  if (sessionCount == 1 && count == 1) {
    closeUp(sessions[first], now);
    OX = 0;
    return;
  }
  sideSky(now);
  floors(first, count, BAND_Y, now);
  char text[12];
  countSign(text, sizeof(text));
  header(text, "户", true, now);
  elevator(pageCount(), (first / T->perPage) * 256, now);
  OX = 0;
}

static uint8_t pageSize(uint8_t p) {
  uint8_t first = p * T->perPage;
  return sessionCount > first ? min<int>(sessionCount - first, T->perPage) : 0;
}

// An elevator ride: the floors pan vertically under the fixed roof while the car rides the cable.
static void transition(uint8_t from, uint8_t to, uint16_t progress, int16_t, unsigned long now) {
  OX = 0;
  int32_t p = progress, e = p < 128 ? 2 * p * p / 256 : 256 - 2 * (256 - p) * (256 - p) / 256;   // ease in-out
  int8_t dir = to > from ? 1 : -1;                    // later pages are further down
  int16_t off = BAND_H * e / 256;
  sideSky(now);
  floors(from * T->perPage, pageSize(from), BAND_Y - dir * off, now);
  floors(to * T->perPage, pageSize(to), BAND_Y + dir * (BAND_H - off), now);
  char text[12];
  countSign(text, sizeof(text));
  header(text, "户", true, now);
  elevator(pageCount(), from * 256 + (to - from) * e, now);
}

static int16_t noPager(uint8_t) { return 0; }

// Nobody home: every window dark, blank plates, the sign blinking 空房 (900 ms on, 300 off)
static void emptyBlock(unsigned long now) {
  OX = 0;
  sideSky(now);
  floors(0, 0, BAND_Y, now);
  // A for-rent card in one dark window
  const int16_t cx = WIN_X[0] + 16, cy = BAND_Y + FLOOR_H + WIN_TOP + 14;
  rect(cx + 8, cy - 8, 1, 8, ROOF);
  rect(cx + 48, cy - 8, 1, 8, ROOF);
  rect(cx, cy, 58, 17, BRASS);
  rect(cx + 1, cy + 1, 56, 15, CREAM);
  drawLabel("等待入住", cx + 5, cy + 13, 48, MORTAR);
  header(nullptr, "空房", (now % 1200) < 900, now);
}

// The settings notice is a second neon sign across the building; long ones break at item gaps
static void notice(const char* text, unsigned long now) {
  char lines[3][64];
  uint8_t count = 0;
  const int16_t maxW = 184;
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
  // Over the top floor's nameplates, covering them whole so no cut text peeks out
  const int16_t w = 204, x = (DISP_W - w) / 2, h = 15 * count + 13, y = BAND_Y + PLATE_Y - 8;
  rect(x - 1, y - 1, w + 2, h + 2, MORTAR);
  rect(x, y, w, h, SKY);
  rect(x + 2, y + 2, w - 4, 1, NEON);
  rect(x + 2, y + h - 3, w - 4, 1, NEON);
  rect(x + 2, y + 3, 1, h - 6, NEON);
  rect(x + w - 3, y + 3, 1, h - 6, NEON);
  rect(x + 4, y + 4, 2, 2, (now / 300) % 2 ? NEON : LAMP);   // a tube end buzzing
  labelFont.setForegroundColor(CREAM);
  for (uint8_t l = 0; l < count; l++) {
    labelFont.drawUTF8((DISP_W - labelFont.getUTF8Width(lines[l])) / 2, y + 18 + 15 * l, lines[l]);
  }
}

const Theme THEME_TOWER = {
  7, "tower", "深夜公寓", "每扇亮灯的窗都是一个会话", 0xFF5FA2, 3, 6, 0,   // id key name tagline glow minFw perPage pageMs
  {
    SKY, BRICK, AMBER, RED,                 // bg gap waitFlash errorFrame
    MORTAR, MORTAR, CREAM, BRASS, BRASS,    // plate tileBorder text tag more
    SKY, NEON, ROOF,                        // pagerBg pagerOn pagerOff
    GREY, BLUE,                             // smoke sleepTarget
    SKY, CREAM,                             // toastBg toastText
    {IDLE, BLUE, LAMP, AMBER, GREEN, RED, BLUE_DK},
    {NEON, TEAL, CREAM, BLUE_DK, PURPLE, GREY},   // unknown agents: none is a room's light
    {NEON, AMBER, CREAM, BLUE, RED, LAMP},
  },
  SKINS, KNOT_LOGO,
  {ICON_GEAR_A, ICON_GEAR_B, ICON_KEYS_A, ICON_KEYS_B, ICON_ASK, ICON_CHECK, ICON_CROSS, ICON_ZZZ},
  {nullptr, nullptr, emptyBlock, nullptr, page, transition, noPager, nullptr, notice, nullptr},
  {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}},
};
