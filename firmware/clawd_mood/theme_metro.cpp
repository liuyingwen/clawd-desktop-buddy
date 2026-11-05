// Theme 6, 地铁线路: the squad as a transit map on warm paper, the only light theme. One red line
// snakes across the page and every session is a station on it: a white disc whose ring and 7x7
// glyph show the state, the agent standing on a platform slab above it, the title printed below.
// A red train stops at the focused station (heldUrgent() first, else each in turn for 3.6 s) and
// the ink bar at the bottom reads 本站 + its title and state. Six stations a page, bigger stations
// with fewer sessions, a hub station for one; pages pan sideways as the line runs on.
// Everything is a pure function of `now` and the session table, so there is no enter hook.
// The theme's own animations divide 1200 ms, the solo train runs 6000 ms and focus turns 3600 ms;
// with the built-in creature's periods (2400 and the error smoke's 360) the squad preview closes
// over 14400 ms and solo and empty over the scenes' 6000.
#include "theme.h"

// 14 colours on a squad frame: keep it at 16 or fewer so a preview sheet stays at 4 bits per pixel.
#define PAPER    RGB(242, 238, 227)
#define GRID     RGB(226, 220, 203)
#define LINE     RGB(226, 65, 43)
#define INK      RGB(30, 30, 36)
#define WHITE    RGB(255, 255, 255)
#define SLAB     RGB(200, 192, 172)
#define GREEN    RGB(46, 158, 87)
#define BLUE     RGB(47, 111, 219)
#define AMBER    RGB(224, 150, 0)
#define GREY     RGB(154, 150, 140)
#define ORANGE   RGB(214, 112, 78)
#define TEAL     RGB(16, 164, 148)
#define DOT_OFF  RGB(84, 84, 92)
#define CUBE_HI  RGB(150, 154, 168)
#define CUBE_MID RGB(90, 94, 108)
#define BELLY    RGB(120, 160, 235)
#define PURPLE   RGB(130, 90, 210)
#define PINK     RGB(214, 72, 140)
#define BROWN    RGB(150, 104, 56)
#define MOSS     RGB(96, 140, 40)
#define SLATE    RGB(70, 110, 150)
#define PLUM     RGB(120, 60, 110)

static const uint8_t I_GEAR_A[7]   = {0x08,0x5D,0x3E,0x77,0x3E,0x5D,0x08};
static const uint8_t I_GEAR_B[7]   = {0x22,0x1C,0x3E,0x36,0x3E,0x1C,0x22};
static const uint8_t I_HAMMER_A[7] = {0x3E,0x3E,0x08,0x08,0x08,0x08,0x00};   // raised
static const uint8_t I_HAMMER_B[7] = {0x41,0x3E,0x3E,0x08,0x08,0x08,0x08};   // struck, two sparks
static const uint8_t I_ASK[7]      = {0x3E,0x63,0x03,0x0E,0x08,0x00,0x08};
static const uint8_t I_CHECK[7]    = {0x00,0x01,0x03,0x46,0x6C,0x38,0x10};
static const uint8_t I_CROSS[7]    = {0x63,0x77,0x3E,0x1C,0x3E,0x77,0x63};
static const uint8_t I_ZZZ[7]      = {0x7F,0x03,0x06,0x0C,0x18,0x30,0x7F};

// Ink on paper. OC's face is the paper itself, so its frame is hollow.
static const Skin SKINS[] = {
  {"CC", {".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB."},
   ORANGE, 0, 0, INK, 0, 11, 2, 8, 2, 3, 7, 2, EYES_SQUARE},
  {"CX", {"..BBBBBBBB..",      // white buddy in an ink outline, the knot as a hat
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          "..BBBBBBBB.."},
   INK, 0, WHITE, INK, 0, 11, 3, 7, 2, 3, 7, 2, EYES_SQUARE, true},
  {"OC", {".BBBBBBBBBB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BBBBBBBBBB."},
   INK, 0, PAPER, INK, 0, 11, 3, 7, 2, 3, 7, 2, EYES_SQUARE},
  {"PI", {"BBBBBBBBB...",
          "BBBFFFFBB...",
          "BBBFFFFBB...",
          "BBBFFFFBB...",
          "BBBBBB...BBB",
          "BBBBBB...BBB",
          "BBB......BBB"},
   INK, 0, WHITE, INK, -1, -1, 0, 9, 3, 4, -1, 1, EYES_SQUARE},
  {"DS", {"..........BB",
          "..BBBBB..BB.",
          ".BBBBBBB.BB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBB..",
          ".AAAAAAA....",
          "..AAAAAA...."},
   BLUE, BELLY, 0, WHITE, 0, -1, 2, 6, 2, 2, -1, 2, EYES_SQUARE},
  {"WB", {".B........B.",      // a solid teal cat
          ".BB......BB.",
          ".BBBBBBBBBB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BBBBBBBBBB."},
   TEAL, 0, TEAL, INK, 0, 11, 3, 7, 2, 3, 7, 3, EYES_SLASH},
  {"CU", {"....AAAA....",      // isometric cube: pale top, mid-grey left, ink right
          "..AAAAAAAA..",
          ".BAAAAAAAAF.",
          ".BBBAAAAFFF.",
          ".BBBBBFFFFF.",
          ".BBBBBFFFFF.",
          "...BBBFFF..."},
   CUBE_MID, CUBE_HI, INK, WHITE, 0, 11, 3, 7, 2, 3, 7, 4, EYES_SQUARE},
};
static_assert(sizeof(SKINS) / sizeof(SKINS[0]) == SKIN_COUNT, "every theme carries all agents");

// ── Geometry ────────────────────────────────────────────────────

#define PER_PAGE  6
#define BAR_H     26
#define MAP_H     (DISP_H - BAR_H)   // 214
#define HALF      3                  // the line is 6 px: rows y-3 .. y+2
#define TURN      12                 // corner radius at the line's centre
#define LEFT_X    18                 // centre of the left and right vertical runs
#define RIGHT_X   222
#define ROW_Y     128                // the one row of a 1-3 station page
#define FOCUS_MS  3600UL
#define ARRIVE_MS 500UL
#define SOLO_MS   6000UL             // the solo train: in, dwell, out

static const int16_t ROWS3[3] = {45, 116, 186};

// A quarter of the line's turn in a 15x15 box, as one run per row: the line comes in from the left
// on rows 0-5 and leaves downwards on columns 9-14. Mirrored for the other three turns.
static const uint8_t TURN_RUN[15][2] = {
  {0, 4}, {0, 7}, {0, 8}, {0, 10}, {0, 11}, {0, 12}, {3, 12}, {5, 13},
  {6, 14}, {7, 14}, {8, 14}, {8, 15}, {9, 15}, {9, 15}, {9, 15},
};

enum Turn : uint8_t { IN_LEFT_DOWN, IN_LEFT_UP, OUT_RIGHT_DOWN, OUT_RIGHT_UP };

// IN_LEFT_DOWN: in from the left on top, out at the bottom (the box's own shape). IN_LEFT_UP: in from
// the left at the bottom, out at the top. OUT_RIGHT_DOWN: in from the top, out to the right at the
// bottom. OUT_RIGHT_UP: in from the bottom, out to the right on top.
static void turn(int16_t bx, int16_t by, Turn t, uint16_t col) {
  bool flipY = t == IN_LEFT_UP || t == OUT_RIGHT_DOWN;
  bool flipX = t == OUT_RIGHT_DOWN || t == OUT_RIGHT_UP;
  for (uint8_t r = 0; r < 15; r++) {
    uint8_t s = TURN_RUN[r][0], e = TURN_RUN[r][1];
    int16_t y = by + (flipY ? 14 - r : r);
    int16_t x0 = flipX ? bx + 15 - e : bx + s;
    canvas.drawFastHLine(x0, y, e - s, col);
  }
}

static void hrun(int16_t x0, int16_t x1, int16_t y, uint16_t col, bool dashed) {
  if (x1 <= x0) return;
  if (!dashed) {
    canvas.fillRect(x0, y - HALF, x1 - x0, 2 * HALF, col);
    return;
  }
  for (int16_t x = x0; x < x1; x += 10) canvas.fillRect(x, y - HALF, min(6, x1 - x), 2 * HALF, col);
}

static void vrun(int16_t x, int16_t y0, int16_t y1, uint16_t col, bool dashed) {
  if (y1 <= y0) return;
  if (!dashed) {
    canvas.fillRect(x - HALF, y0, 2 * HALF, y1 - y0, col);
    return;
  }
  for (int16_t y = y0; y < y1; y += 10) canvas.fillRect(x - HALF, y, 2 * HALF, min(6, y1 - y), col);
}

// How a page's line ends on each side: a terminus, off the screen's edge, or at a page-change seam
// where connect() takes over 12 px short of it
enum End : uint8_t { END_CAP, END_EDGE, END_SEAM, END_JOIN };

static int16_t endX(End e, bool right) {
  switch (e) {
    case END_CAP:  return right ? DISP_W - 10 : 10;
    case END_SEAM: return right ? DISP_W - TURN : TURN;
    default:       return right ? DISP_W : 0;
  }
}

// The route: one row, or three rows snaking right, down, left, down, right
static void route(uint8_t rows, int16_t ox, End left, End right, uint16_t col, bool dashed) {
  int16_t x0 = ox + endX(left, false), x1 = ox + endX(right, true);
  if (rows == 1) {
    hrun(x0, x1, ROW_Y, col, dashed);
  } else {
    const int16_t* Y = ROWS3;
    int16_t rx = ox + RIGHT_X, lx = ox + LEFT_X;
    hrun(x0, rx - TURN, Y[0], col, dashed);
    turn(rx - TURN, Y[0] - HALF, IN_LEFT_DOWN, col);
    vrun(rx, Y[0] + TURN, Y[1] - TURN, col, dashed);
    turn(rx - TURN, Y[1] - TURN, IN_LEFT_UP, col);
    hrun(lx + TURN, rx - TURN, Y[1], col, dashed);
    turn(lx - HALF, Y[1] - HALF, OUT_RIGHT_UP, col);
    vrun(lx, Y[1] + TURN, Y[2] - TURN, col, dashed);
    turn(lx - HALF, Y[2] - TURN, OUT_RIGHT_DOWN, col);
    hrun(lx + TURN, x1, Y[2], col, dashed);
  }
  int16_t yl = rows == 1 ? ROW_Y : ROWS3[0], yr = rows == 1 ? ROW_Y : ROWS3[2];
  if (left == END_CAP) canvas.fillRoundRect(x0 - 2, yl - HALF, 10, 2 * HALF, HALF, col);
  if (right == END_CAP) canvas.fillRoundRect(x1 - 8, yr - HALF, 10, 2 * HALF, HALF, col);
  if (right == END_EDGE && !dashed) {   // two white chevrons: the line runs on to the next page
    for (uint8_t i = 0; i < 2; i++) {
      int16_t cx = ox + DISP_W - 14 + 5 * i;
      canvas.drawFastVLine(cx, yr - 2, 4, WHITE);
      canvas.drawFastVLine(cx + 1, yr - 1, 2, WHITE);
    }
  }
}

// The page-change seam at x: the old page's last row (ya) bends into the new page's first row (yb)
static void connect(int16_t x, int16_t ya, int16_t yb) {
  if (ya == yb) return;
  if (yb < ya) {
    turn(x - TURN, ya - TURN, IN_LEFT_UP, LINE);
    canvas.fillRect(x - HALF, yb + TURN, 2 * HALF, ya - yb - 2 * TURN, LINE);
    turn(x - HALF, yb - HALF, OUT_RIGHT_UP, LINE);
  } else {
    turn(x - TURN, ya - HALF, IN_LEFT_DOWN, LINE);
    canvas.fillRect(x - HALF, ya + TURN, 2 * HALF, yb - ya - 2 * TURN, LINE);
    turn(x - HALF, yb - TURN, OUT_RIGHT_DOWN, LINE);
  }
}

static int16_t firstY(uint8_t n) { return n >= 4 ? ROWS3[0] : ROW_Y; }
static int16_t lastY(uint8_t n) { return n >= 4 ? ROWS3[2] : ROW_Y; }

// ── Stations ────────────────────────────────────────────────────

struct Stop {
  int16_t x, y;      // centre, on the line
  int8_t dir;        // the way the line runs here: 1 right, -1 left
  int16_t colL, colR;
};

struct Scale {
  uint8_t u, fu, pu;   // creature, passenger, prop units
  uint8_t r, ring, gu; // station radius, ring width, glyph unit
  uint8_t slabH, tu;   // platform thickness, train unit
};

static Scale scaleOf(uint8_t n) {
  if (n >= 4) return {2, 1, 1, 10, 3, 1, 3, 1};
  if (n == 3) return {3, 1, 1, 13, 3, 2, 3, 1};
  if (n == 2) return {4, 1, 2, 14, 3, 2, 4, 2};
  return {6, 2, 2, 24, 3, 2, 5, 2};
}

// Rows of 4-6 stations: 1-2-1, 2-1-2, 2-2-2. Order follows the line, so the middle row runs right to left.
static Stop stopOf(uint8_t i, uint8_t n) {
  if (n <= 3) {
    static const int16_t X3[3] = {42, 120, 198}, X2[2] = {64, 176};
    int16_t w = n == 3 ? 76 : n == 2 ? 108 : 200;
    int16_t x = n == 3 ? X3[i] : n == 2 ? X2[i] : 120;
    return {x, ROW_Y, 1, (int16_t)(x - w / 2), (int16_t)(x + w / 2)};
  }
  static const uint8_t PER_ROW[3][3] = {{1, 2, 1}, {2, 1, 2}, {2, 2, 2}};
  const uint8_t* per = PER_ROW[n - 4];
  uint8_t row = 0;
  while (i >= per[row]) i -= per[row++];
  int8_t dir = row == 1 ? -1 : 1;
  int16_t x;
  if (per[row] == 1) {
    x = 120;
    return {x, ROWS3[row], dir, 30, 210};
  }
  uint8_t k = dir > 0 ? i : 1 - i;
  x = k ? 166 : 74;
  return {x, ROWS3[row], dir, (int16_t)(k ? 122 : 30), (int16_t)(k ? 210 : 118)};
}

// 12 points on a circle, x1000
static const int16_t RING_X[12] = {1000, 866, 500, 0, -500, -866, -1000, -866, -500, 0, 500, 866};
static const int16_t RING_Y[12] = {0, 500, 866, 1000, 866, 500, 0, -500, -866, -1000, -866, -500};

static void glyph(int16_t x, int16_t y, const uint8_t* rows, uint8_t u, uint16_t col) {
  for (uint8_t r = 0; r < 7; r++) {
    for (uint8_t c = 0; c < 7;) {
      if (!(rows[r] & (0x40 >> c))) { c++; continue; }
      uint8_t e = c + 1;
      while (e < 7 && (rows[r] & (0x40 >> e))) e++;
      canvas.fillRect(x + c * u, y + r * u, (e - c) * u, u, col);
      c = e;
    }
  }
}

static Mood shown(const Session& s) { return s.mood < MOOD_UNKNOWN ? s.mood : MOOD_IDLE; }

// The disc: white, a ring in the state's colour, the state's glyph. hub: the solo double ring.
static void station(int16_t cx, int16_t cy, const Scale& k, Mood m, bool alert, unsigned long since,
                    bool hub, unsigned long now) {
  uint16_t ring = stateColor(m);
  if (m == MOOD_WAITING && alert) {   // a dashed amber halo that ticks round
    uint8_t phase = (now / 300) % 2, d = k.gu + 1;
    int16_t rr = k.r + 3 + k.gu;
    for (uint8_t i = phase; i < 12; i += 2) {
      canvas.fillRect(cx + RING_X[i] * rr / 1000 - d / 2, cy + RING_Y[i] * rr / 1000 - d / 2, d, d, AMBER);
    }
  }
  if (m == MOOD_DONE && now - since < 900) {   // a burst of confetti off the disc
    int16_t rr = k.r + 3 + (now - since) / 150;
    for (uint8_t i = 1; i < 12; i += 2) {
      canvas.fillRect(cx + RING_X[i] * rr / 1000 - 1, cy + RING_Y[i] * rr / 1000 - 1, 2, 2,
                      T->pal.confetti[i / 2]);
    }
  }
  int16_t r = k.r;
  if (hub) {   // an ink outer ring, a white gap, then the state ring
    canvas.fillCircle(cx, cy, r, INK);
    canvas.fillCircle(cx, cy, r - 3, WHITE);
    r -= 5;
  }
  if (m == MOOD_ERROR && alert && (now / 300) % 2) ring = INK;
  canvas.fillCircle(cx, cy, r, ring);
  uint8_t gu = k.gu;
  if (m == MOOD_WAITING) {   // the one solid disc on the map, an ink question mark in it
    glyph(cx - 7 * gu / 2, cy - 7 * gu / 2, T->icons.ask, gu, INK);
    return;
  }
  canvas.fillCircle(cx, cy, r - k.ring, WHITE);
  if (m == MOOD_IDLE) {
    canvas.fillRect(cx - 3 * gu / 2, cy - 3 * gu / 2, 3 * gu, 3 * gu, INK);
    return;
  }
  const uint8_t* g = iconFor(m, now);
  if (g) glyph(cx - 7 * gu / 2, cy - 7 * gu / 2, g, gu, stateColor(m));
}

static void textAt(const char* s, int16_t x, int16_t y, uint8_t size, uint16_t col) {
  canvas.setTextSize(size);
  canvas.setTextColor(col);
  canvas.setCursor(x, y);
  canvas.print(s);
}

// ── The train ───────────────────────────────────────────────────

// 22x10 units sitting on the line at y, nose towards dir. lit: windows on.
static void train(int16_t x, int16_t y, uint8_t u, int8_t dir, bool lit) {
  int16_t top = y - 7 * u, w = 22 * u;
  canvas.fillRoundRect(x, top, w, 8 * u, 2 * u, INK);
  canvas.fillRoundRect(x + u, top + u, w - 2 * u, 6 * u, u, LINE);
  uint16_t glass = lit ? WHITE : INK;
  int16_t nose = dir > 0 ? x + w - 5 * u : x + 2 * u;
  canvas.fillRect(nose, top + 2 * u, 3 * u, 3 * u, glass);
  for (uint8_t i = 0; i < 3; i++) {
    int16_t wx = dir > 0 ? x + 2 * u + i * 5 * u : x + 7 * u + i * 5 * u;
    canvas.fillRect(wx, top + 2 * u, 3 * u, 2 * u, glass);
  }
  canvas.fillRect(x + 3 * u, y + u, 3 * u, 2 * u, INK);
  canvas.fillRect(x + w - 6 * u, y + u, 3 * u, 2 * u, INK);
}

// ── Focus ───────────────────────────────────────────────────────

static unsigned long landedAt() { return slideStartMs + SLIDE_FRAMES * SLIDE_FRAME_MS; }

// The rotation's clock: while a page change is on, the moment it lands, so the incoming page
// already names the station the train will stop at and nothing switches when it lands
static unsigned long focusClock(unsigned long now) { return slideTo >= 0 ? landedAt() : now; }

static uint8_t focusOf(uint8_t first, uint8_t n, unsigned long now) {
  int8_t held = heldUrgent(now);
  if (held >= first && held < first + n) return held - first;
  return (focusClock(now) / FOCUS_MS) % n;
}

// When the train sets off for the focused station: the urgent turn, the rotation, or the landing
// of a page change, whichever is last. During a page change that is still ahead, so the train
// waits out of the station and pulls in once, after the page lands.
static unsigned long arrivalOf(uint8_t first, uint8_t n, unsigned long now) {
  int8_t held = heldUrgent(now);
  unsigned long clock = focusClock(now);
  unsigned long t = held >= first && held < first + n ? sessions[held].moodSinceMs : clock - clock % FOCUS_MS;
  unsigned long slid = landedAt();
  if ((long)(slid - t) > 0 && (long)(now - slideStartMs) >= 0) t = slid;
  return t;
}

// ── A station's platform ────────────────────────────────────────

static uint8_t passengersFit(uint8_t n, uint8_t fu, int16_t room, int16_t* width) {
  int16_t pitch = 12 * fu + 1, badge = n >= 10 ? 20 : 14;
  uint8_t fit = n * pitch <= room ? n : (room - badge) / pitch > 0 ? (room - badge) / pitch : 0;
  *width = n ? fit * pitch + (fit < n ? badge : 0) : 0;
  return fit;
}

static void passengers(const Session& s, uint8_t fit, int16_t x, int16_t feet, uint8_t fu, unsigned long now) {
  int16_t pitch = 12 * fu + 1;
  for (uint8_t i = 0; i < fit; i++) {
    const Follower& f = s.followers[i];
    unsigned long age = now - f.bornMs;
    int16_t drop = age < 400 ? (400 - age) / 40 : 0;
    drawCreature(s.src, s.color, f.mood, f.moodSinceMs, f.mood == MOOD_WAITING, x + i * pitch, feet - drop,
                 fu, now + i * 110);
  }
  uint8_t extra = s.followerCount - fit;
  if (extra) {
    char buf[5];
    snprintf(buf, sizeof(buf), "+%u", extra);
    textAt(buf, x + fit * pitch + 1, feet - 8, 1, INK);
  }
}

// What stands beside the creature: a laptop while working, a "?" sign on a post while waiting
static void prop(Mood m, bool alert, int16_t x, int16_t feet, uint8_t p, unsigned long now) {
  if (m == MOOD_WORKING) {   // a laptop seen from the front, its screen scrolling
    canvas.fillRect(x + p, feet - 8 * p, 8 * p, 6 * p, INK);
    canvas.fillRect(x + 2 * p, feet - 7 * p, 6 * p, 4 * p, GREEN);
    uint8_t line = (now / 150) % 4;
    canvas.fillRect(x + 3 * p, feet - 7 * p + line * p, (line % 2 ? 2 : 4) * p, p, WHITE);
    canvas.fillRect(x, feet - 2 * p, 10 * p, 2 * p, GREY);
  } else if (m == MOOD_WAITING) {
    int16_t bob = alert ? ((now / 300) % 2) * p : 0;
    int16_t sx = x, sy = feet - 17 * p - bob;
    canvas.fillRect(x + 4 * p, sy + 9 * p, p, feet - sy - 9 * p, INK);    // post
    canvas.fillRect(sx, sy, 9 * p, 9 * p, AMBER);
    glyph(sx + p, sy + p, I_ASK, p, INK);
  }
}

// A striped barrier across the platform, in front of the creature
static void barrier(int16_t x0, int16_t x1, int16_t feet, uint8_t p) {
  int16_t h = 3 * p, y = feet - h - 2 * p;
  canvas.fillRect(x0, y, x1 - x0, h, WHITE);
  for (uint8_t r = 0; r < h; r++) {
    for (int16_t x = x0 - 8 + (r % 8); x < x1; x += 8) {
      int16_t a = max(x, x0), b = min((int16_t)(x + 4), x1);
      if (b > a) canvas.drawFastHLine(a, y + r, b - a, LINE);
    }
  }
  canvas.fillRect(x0, y - p, p + 1, h + 3 * p, INK);
  canvas.fillRect(x1 - p - 1, y - p, p + 1, h + 3 * p, INK);
}

// Platform, agent, passengers and props of one station, centred over (cx) within its column
static void platform(const Session& s, const Stop& st, const Scale& k, int16_t ox, unsigned long now) {
  Mood m = shown(s);
  bool alert = isAlerting(s);
  int16_t feet = st.y - k.r - k.slabH - 2;
  int16_t propW = 10 * k.pu, body = 12 * k.u;
  int16_t colL = ox + st.colL, colR = ox + st.colR;
  int16_t pw = 0;
  uint8_t fit = passengersFit(s.followerCount, k.fu, colR - colL - propW - body - 2, &pw);
  int16_t groupW = propW + body + (s.followerCount ? 2 + pw : 0);
  int16_t gx = max(colL, min((int16_t)(ox + st.x - groupW / 2), (int16_t)(colR - groupW)));
  canvas.fillRect(gx - 2, feet, groupW + 4, k.slabH, SLAB);
  canvas.drawFastHLine(gx - 2, feet + k.slabH - 1, groupW + 4, GREY);
  int16_t bx = gx + propW;
  prop(m, alert, gx, feet, k.pu, now);
  drawCreature(s.src, s.color, m, s.moodSinceMs, false, bx, feet, k.u, now);
  if (s.followerCount) passengers(s, fit, bx + body + 2, feet, k.fu, now);
  uint8_t ts = k.u >= 4 ? 2 : 1;
  if (m == MOOD_SLEEPING) {   // a z drifting up off the sleeper
    uint16_t t = now % 1200;
    textAt("z", bx + body - 2 * k.u + t / 400 * ts, feet - 6 * k.u - 7 * ts - t * 4 * ts / 1200, ts, GREY);
  } else if (m == MOOD_THINKING) {   // three dots coming one by one
    uint8_t dots = (now / 300) % 4, d = k.u;
    for (uint8_t i = 0; i < dots; i++) {
      canvas.fillRect(bx + body + i * (d + 2) - d, feet - 11 * k.u - i * d, d, d, BLUE);
    }
  } else if (m == MOOD_ERROR) {
    barrier(gx - 2, gx + groupW + 2, feet, k.pu);
  }
}

// Name board under the station; the focused one is ink with white letters
static void nameBoard(const Session& s, const Stop& st, const Scale& k, int16_t ox, bool focused) {
  char label[sizeof(s.title)];
  int16_t w = fitLabel(label, sizeof(label), labelOf(s), st.colR - st.colL - 6);
  int16_t x = ox + max((int16_t)(st.colL + 3), min((int16_t)(st.x - w / 2), (int16_t)(st.colR - 3 - w)));
  int16_t top = st.y + k.r + 4;
  uint16_t col = shown(s) == MOOD_SLEEPING ? GREY : INK;
  if (focused) {
    canvas.fillRect(x - 3, top, w + 6, 14, INK);
    col = WHITE;
  }
  labelFont.setForegroundColor(col);
  labelFont.drawUTF8(x, top + 11, label);
}

// ── A page ──────────────────────────────────────────────────────

static void paper(int16_t ox) {
  canvas.fillRect(0, 0, DISP_W, MAP_H, PAPER);
  int16_t phase = ((ox % 8) + 8) % 8 + 3;
  for (int16_t y = 3; y < MAP_H; y += 8) {
    for (int16_t x = phase; x < DISP_W; x += 8) canvas.writePixel(x, y, GRID);
  }
}

// Amber and ink stripes marching along the top while a station on this page needs approval
static void hazard(int16_t ox, unsigned long now) {
  int16_t shift = (now / 150) % 8;
  canvas.fillRect(ox, 0, DISP_W, 4, INK);
  for (uint8_t r = 0; r < 4; r++) {
    for (int16_t x = ox - 8 + (shift + r) % 8; x < ox + DISP_W; x += 8) {
      canvas.drawFastHLine(x, r, 4, AMBER);
    }
  }
}

// The solo train pulls in, waits, and leaves behind the hub, every SOLO_MS
static int16_t soloTrainX(unsigned long now) {
  const int16_t stop = 120 - 24 - 2 - 44;
  unsigned long t = now % SOLO_MS;
  if (t < 1200) {
    int32_t q = 1200 - t;
    return stop - (int16_t)(130 * q * q / (1200L * 1200));
  }
  if (t < 4800) return stop;
  int32_t q = t - 4800;
  return stop + (int16_t)(200 * q * q / (1200L * 1200));
}

static void drawMap(uint8_t first, uint8_t n, int16_t ox, End left, End right, bool withTrain,
                    unsigned long now) {
  if (!n) return;
  Scale k = scaleOf(n);
  bool alarm = false;
  for (uint8_t i = 0; i < n; i++) alarm |= sessions[first + i].mood == MOOD_WAITING && isAlerting(sessions[first + i]);
  if (alarm) hazard(ox, now);
  route(n >= 4 ? 3 : 1, ox, left, right, LINE, false);

  uint8_t focus = focusOf(first, n, now);
  Stop fs = stopOf(focus, n);
  if (withTrain && n == 1) {
    train(ox + soloTrainX(now), ROW_Y, k.tu, 1, true);
  } else if (withTrain) {
    long dt = (long)(now - arrivalOf(first, n, now));   // negative: waiting for the page to land
    int32_t q = dt < 0 ? ARRIVE_MS : dt < (long)ARRIVE_MS ? ARRIVE_MS - dt : 0;
    int16_t back = (int16_t)(40 * q * q / ((int32_t)ARRIVE_MS * ARRIVE_MS));
    int16_t w = 22 * k.tu;
    int16_t tx = fs.dir > 0 ? fs.x - k.r - 1 - w - back : fs.x + k.r + 1 + back;
    train(ox + tx, fs.y, k.tu, fs.dir, true);
  }
  for (uint8_t i = 0; i < n; i++) {
    const Session& s = sessions[first + i];
    Stop st = stopOf(i, n);
    station(ox + st.x, st.y, k, shown(s), isAlerting(s), s.moodSinceMs, n == 1, now);
  }
  for (uint8_t i = 0; i < n; i++) {
    const Session& s = sessions[first + i];
    Stop st = stopOf(i, n);
    platform(s, st, k, ox, now);
    nameBoard(s, st, k, ox, i == focus && withTrain);
  }
}

static End leftEnd() { return pageCount() > 1 ? END_EDGE : END_CAP; }
static End rightEnd() { return pageCount() > 1 ? END_EDGE : END_CAP; }

static void page(uint8_t first, uint8_t count, int16_t ox, int16_t, unsigned long now) {
  paper(ox);
  drawMap(first, count, ox, leftEnd(), rightEnd(), true, now);
}

static uint8_t countOn(uint8_t p) {
  uint8_t first = p * PER_PAGE;
  return sessionCount > first ? min(sessionCount - first, PER_PAGE) : 0;
}

// The map pans left and the line bends across the seam into the next page
static void transition(uint8_t from, uint8_t to, uint16_t progress, int16_t, unsigned long now) {
  int32_t rest = TRANSITION_ONE - progress;
  int16_t shift = DISP_W - (int16_t)(DISP_W * rest * rest / (TRANSITION_ONE * TRANSITION_ONE));
  uint8_t a = countOn(from), b = countOn(to);
  int16_t seam = DISP_W - shift;
  int16_t ya = lastY(a), yb = firstY(b);
  paper(-shift);
  drawMap(from * PER_PAGE, a, -shift, leftEnd(), ya == yb ? END_JOIN : END_SEAM, false, now);
  drawMap(to * PER_PAGE, b, seam, ya == yb ? END_JOIN : END_SEAM, rightEnd(), true, now);
  connect(seam, ya, yb);
}

// ── The platform sign at the bottom ─────────────────────────────

static const char* const WORDS[7] = {"空闲", "思考中", "工作中", "等你批准", "完成", "出错", "睡眠中"};

static int16_t pagerHeight(uint8_t) { return BAR_H; }

static void bar(uint8_t pages, uint8_t active, int16_t y, int16_t h, unsigned long now) {
  uint8_t first = active * PER_PAGE, n = countOn(active);
  canvas.fillRect(0, y, DISP_W, h, INK);
  if (!n) return;
  const Session& s = sessions[first + focusOf(first, n, now)];
  Mood m = shown(s);
  bool flash = isAlerting(s) && m == MOOD_WAITING && (now / 300) % 2;
  uint16_t fg = flash ? INK : WHITE;
  if (flash) canvas.fillRect(0, y, DISP_W, h, AMBER);
  canvas.fillRect(6, y + 6, 4, 14, flash ? INK : LINE);
  int16_t base = y + 18;
  labelFont.setForegroundColor(flash ? INK : GREY);
  labelFont.drawUTF8(15, base, "本站");

  const char* word = WORDS[m];
  int16_t ww = labelFont.getUTF8Width(word);
  int16_t wx = DISP_W - 6 - ww, ix = wx - 11;
  uint16_t wc = flash ? INK : m == MOOD_IDLE ? PAPER : stateColor(m);
  bool blinkOff = m == MOOD_ERROR && isAlerting(s) && (now / 300) % 2;
  if (!blinkOff) {
    const uint8_t* g = m == MOOD_IDLE ? nullptr : iconFor(m, now);
    if (g) glyph(ix, y + 9, g, 1, wc);
    else canvas.fillRect(ix + 2, y + 11, 3, 3, wc);
    labelFont.setForegroundColor(wc);
    labelFont.drawUTF8(wx, base, word);
  }
  int16_t right = ix - 6;
  if (pages > 1) {   // page dots in the line's colour; a page with an alert blinks in its state colour
    const int16_t d = 4, gap = 4;
    int16_t dx = right - pages * (d + gap) + gap;
    for (uint8_t p = 0; p < pages; p++) {
      uint16_t col = p == active ? LINE : DOT_OFF;
      for (uint8_t i = p * PER_PAGE; i < sessionCount && i < (p + 1) * PER_PAGE; i++) {
        if (p != active && isAlerting(sessions[i]) && (now / 300) % 2) col = stateColor(sessions[i].mood);
      }
      canvas.fillRect(dx + p * (d + gap), y + 11, d, d, col);
    }
    right = dx - 6;
  }
  char label[sizeof(s.title)];
  fitLabel(label, sizeof(label), labelOf(s), right - 44);
  labelFont.setForegroundColor(fg);
  labelFont.drawUTF8(44, base, label);
}

// ── No sessions: the line out of service ────────────────────────

static void outOfService(unsigned long now) {
  paper(0);
  route(3, 0, END_CAP, END_CAP, GREY, true);
  Scale k = scaleOf(6);
  for (uint8_t i = 0; i < 6; i++) {
    Stop st = stopOf(i, 6);
    canvas.fillCircle(st.x, st.y, k.r, GREY);
    canvas.fillCircle(st.x, st.y, k.r - 2, WHITE);
    canvas.fillRect(st.x - 12, st.y + k.r + 6, 24, 3, SLAB);   // a blank name board
  }
  // The train parked at the terminus, lights off; a signal beside it blinks red
  train(10, ROWS3[0], 1, 1, false);
  canvas.fillRect(37, ROWS3[0] - 18, 1, 15, INK);
  canvas.fillRect(35, ROWS3[0] - 22, 5, 5, INK);
  if ((now / 600) % 2 == 0) canvas.fillRect(36, ROWS3[0] - 21, 3, 3, LINE);

  int16_t y = MAP_H;
  canvas.fillRect(0, y, DISP_W, BAR_H, INK);
  canvas.fillRect(6, y + 6, 4, 14, GREY);
  labelFont.setForegroundColor(PAPER);
  labelFont.drawUTF8(15, y + 18, "等待会话接入");
  uint8_t lit = (now / 400) % 3;
  for (uint8_t i = 0; i < 3; i++) canvas.fillRect(DISP_W - 26 + i * 6, y + 12, 3, 3, i == lit ? AMBER : DOT_OFF);
}

// ── Settings confirmation: an ink sign with a station marker ────

// Over the middle row of a 4-6 station page. A page of 1-3 stations ends its name boards by y 170,
// so the sign goes on the blank paper above the bar instead of cutting through agents and discs.
static void notice(const char* text, unsigned long) {
  uint8_t shown = countOn(slideTo >= 0 ? (uint8_t)slideTo : currentPage);
  const int16_t h = 34, x = 6, w = DISP_W - 12, y = shown >= 4 ? 90 : MAP_H - h - 4;
  canvas.fillRect(x - 2, y - 2, w + 4, h + 4, PAPER);
  canvas.fillRect(x, y, w, h, INK);
  canvas.fillRect(x, y, w, 4, LINE);
  int16_t tw = labelFont.getUTF8Width(text);
  int16_t room = w - 8;
  int16_t tx;
  if (tw + 22 <= room) {   // a little station marker before the text
    int16_t gx = x + (w - tw - 22) / 2;
    canvas.fillCircle(gx + 7, y + 20, 6, LINE);
    canvas.fillCircle(gx + 7, y + 20, 3, WHITE);
    tx = gx + 22;
  } else {
    tx = x + max(4, (w - tw) / 2);
  }
  labelFont.setForegroundColor(WHITE);
  labelFont.drawUTF8(tx, y + 25, text);
}

const Theme THEME_METRO = {
  6, "metro", "地铁线路", "每个会话是线路图上的一站", 0xE2412B, 3, PER_PAGE,
  // A page cycle of 6895 + 305 = 7200 ms on the host: two focus turns a page, and the period of
  // the squad scene's built-in animations (the error smoke's 360 ms among them).
  6895,
  {
    PAPER, GRID, AMBER, LINE,              // bg gap waitFlash errorFrame
    WHITE, INK, INK, INK, INK,             // plate tileBorder text tag more
    INK, LINE, DOT_OFF,                    // pagerBg pagerOn pagerOff
    DOT_OFF, GREY,                         // smoke sleepTarget: sleepers grey out
    INK, WHITE,                            // toastBg toastText
    {INK, BLUE, GREEN, AMBER, TEAL, LINE, GREY},
    {PURPLE, PINK, BROWN, MOSS, SLATE, PLUM},
    {LINE, AMBER, GREEN, BLUE, TEAL, ORANGE},
  },
  SKINS, KNOT_LOGO,
  {I_GEAR_A, I_GEAR_B, I_HAMMER_A, I_HAMMER_B, I_ASK, I_CHECK, I_CROSS, I_ZZZ},
  {nullptr, nullptr, outOfService, nullptr, page, transition, pagerHeight, bar, notice, nullptr},
  // The squad scene's error session holds the focus for 10 s: settle past it, then four focus turns.
  {{12000, 14400, 10}, {0, 0, 0}, {0, 0, 0}},
};
