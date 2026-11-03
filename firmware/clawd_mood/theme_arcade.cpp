// Theme 4, 街机: the whole squad is one Space Invaders formation on a night sky, all on one
// screen (perPage 12, so no pages). A player ship below slides under the focused invader and a
// marquee names it. Every animation period divides 2400 ms (focus rotation, 2400 ms a session)
// or the 9600 ms parade, so previews loop. The only state kept is the ship's position, reset in enter.
#include "theme.h"

#define INDIGO  RGB(11, 6, 32)
#define NIGHT   RGB(44, 34, 92)
#define WHITE   RGB(255, 255, 255)
#define MAGENTA RGB(255, 62, 165)
#define CYAN    RGB(62, 232, 255)
#define YELLOW  RGB(255, 225, 77)
#define RED     RGB(255, 59, 78)
#define LIME    RGB(124, 255, 79)
#define GREY    RGB(108, 104, 144)
#define SILVER  RGB(201, 196, 240)
#define ORANGE  RGB(255, 138, 61)
#define VIOLET  RGB(176, 124, 255)
#define BLUE    RGB(79, 123, 255)
#define TEAL    RGB(31, 229, 181)

static const uint8_t I_GEAR_A[7] = {0x08,0x5D,0x3E,0x77,0x3E,0x5D,0x08};
static const uint8_t I_GEAR_B[7] = {0x22,0x1C,0x3E,0x36,0x3E,0x1C,0x22};
static const uint8_t I_KEYS_A[7] = {0x00,0x7F,0x55,0x7F,0x6B,0x7F,0x00};
static const uint8_t I_KEYS_B[7] = {0x00,0x7F,0x6B,0x7F,0x55,0x7F,0x00};
static const uint8_t I_ASK[7]    = {0x3E,0x63,0x03,0x0E,0x08,0x00,0x08};
static const uint8_t I_CHECK[7]  = {0x00,0x01,0x03,0x46,0x6C,0x38,0x10};
static const uint8_t I_CROSS[7]  = {0x63,0x77,0x3E,0x1C,0x3E,0x77,0x63};
static const uint8_t I_ZZZ[7]    = {0x7F,0x03,0x06,0x0C,0x18,0x30,0x7F};
static const uint8_t I_IDLE[7]   = {0x00,0x00,0x00,0x7F,0x00,0x00,0x00};

// The same logo silhouettes in neon. 'F' painted INDIGO is a hole (OC's frame, PI's counter).
// CX's accent is the colour of its knot hat (no 'A' in its rows).
static const Skin SKINS[] = {
  {"CC", {".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB."},
   ORANGE, ORANGE, ORANGE, INDIGO, 0, 11, 2, 8, 2, 3, 7, 2, EYES_SQUARE},
  {"CX", {"..BBBBBBBB..",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          "..BBBBBBBB.."},
   WHITE, CYAN, WHITE, INDIGO, 0, 11, 3, 7, 2, 3, 7, 2, EYES_SQUARE, true},
  {"OC", {".BBBBBBBBBB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BBBBBBBBBB."},
   SILVER, SILVER, INDIGO, WHITE, 0, 11, 3, 7, 2, 3, 7, 2, EYES_SQUARE},
  {"PI", {"BBBBBBBBB...",
          "BBBFFFFBB...",
          "BBBFFFFBB...",
          "BBBFFFFBB...",
          "BBBBBB...BBB",
          "BBBBBB...BBB",
          "BBB......BBB"},
   VIOLET, VIOLET, INDIGO, WHITE, -1, -1, 0, 9, 3, 4, -1, 1, EYES_SQUARE},
  {"DS", {"..........BB",
          "..BBBBB..BB.",
          ".BBBBBBB.BB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBB..",
          ".AAAAAAA....",
          "..AAAAAA...."},
   BLUE, WHITE, BLUE, WHITE, 0, -1, 2, 6, 2, 2, -1, 2, EYES_SQUARE},
  {"WB", {".B........B.",
          ".BB......BB.",
          ".BBBBBBBBBB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BBBBBBBBBB."},
   TEAL, TEAL, TEAL, INDIGO, 0, 11, 3, 7, 2, 3, 7, 3, EYES_SLASH},
  {"CU", {"....AAAA....",      // isometric cube: white top, silver and grey sides
          "..AAAAAAAA..",
          ".BAAAAAAAAF.",
          ".BBBAAAAFFF.",
          ".BBBBBFFFFF.",
          ".BBBBBFFFFF.",
          "...BBBFFF..."},
   SILVER, WHITE, GREY, INDIGO, 0, 11, 3, 7, 2, 3, 7, 4, EYES_SQUARE},
};
static_assert(sizeof(SKINS) / sizeof(SKINS[0]) == SKIN_COUNT, "every theme carries all agents");

// ── Screen layout ───────────────────────────────────────────────

#define HUD_Y      2
#define SKY_TOP    20
#define SKY_BOTTOM 179     // feet of the lowest row plus its dive stay above this
#define SHIP_Y     182
#define GROUND_Y   197
#define TITLE_BASE 216
#define PIPS_Y     226
#define FOCUS_MS   2400UL  // each session's turn on the marquee
#define DROP_MS    400UL   // a new escort falls into place

static const int8_t MARCH[8] = {0, 1, 2, 1, 0, -1, -2, -1};

struct Slot {
  int16_t x, w;        // the column this invader may use
  int16_t feet;        // bottom of its legs when not diving
  uint8_t u, fu;       // invader and escort units
  int16_t step, dive;  // march step and how far a waiting invader drops, px
  uint8_t row;
  int16_t ceiling;     // escorts stay below this: the HUD text, or the deepest dive of the row above
};

// 1 = a boss, 2-4 = one row, 5-8 = two rows, 9-12 = three rows; short rows are centred.
static Slot slotOf(uint8_t i, uint8_t n) {
  Slot s;
  uint8_t rows = n <= 4 ? 1 : n <= 8 ? 2 : 3;
  uint8_t cols = (n + rows - 1) / rows;
  uint8_t r = i / cols, c = i % cols;
  uint8_t inRow = min((int)cols, n - r * cols);
  s.row = r;
  s.w = DISP_W / cols;
  s.x = (DISP_W - inRow * s.w) / 2 + c * s.w;
  if (rows == 1) {
    static const uint8_t U[5] = {8, 8, 6, 5, 4};
    static const int16_t FEET[5] = {168, 168, 152, 142, 134};
    s.u = U[n];
    s.fu = n <= 2 ? 2 : 1;
    s.feet = FEET[n];
    s.dive = s.u;
  } else {
    int16_t band = (SKY_BOTTOM - SKY_TOP) / rows;
    s.u = rows == 2 ? 4 : 3;
    s.fu = 1;
    s.dive = rows == 2 ? 4 : 3;
    s.feet = SKY_TOP + (r + 1) * band - s.dive;
  }
  s.ceiling = r ? SKY_TOP + r * ((SKY_BOTTOM - SKY_TOP) / rows) : HUD_Y + 16;
  s.step = max(1, min((int)s.u, (s.w - 12 * s.u - 4) / 4));
  return s;
}

// ── Drawing an invader ──────────────────────────────────────────

struct Paint { uint16_t body, accent, face, eye, hat; };
enum EyeMode : uint8_t { EYE_OPEN, EYE_BLINK, EYE_HAPPY, EYE_X };

// One torso row as runs of equal cells, not cell by cell
static void torsoRow(const char* row, int16_t x, int16_t y, uint8_t u, const Paint& p, bool shimmer) {
  for (uint8_t c = 0; c < 12;) {
    char ch = row[c];
    if (ch != 'B' && ch != 'A' && ch != 'F') { c++; continue; }
    uint8_t e = c + 1;
    while (e < 12 && row[e] == ch) e++;
    uint16_t col = ch == 'B' ? p.body : ch == 'A' ? p.accent : p.face;
    if (shimmer && col != INDIGO) col = CYAN;
    canvas.fillRect(x + c * u, y, (e - c) * u, u, col);
    c = e;
  }
}

// The Codex knot from T->knot, as horizontal spans
static void knot(int16_t x, int16_t y, int16_t size, uint16_t col) {
  for (int16_t py = 0; py < size; py++) {
    uint32_t row = T->knot[py * 30 / size];
    int16_t run = -1;
    for (int16_t px = 0; px <= size; px++) {
      bool on = px < size && (row & (1UL << (29 - px * 30 / size)));
      if (on && run < 0) run = px;
      if (!on && run >= 0) {
        canvas.drawFastHLine(x + run, y + py, px - run, col);
        run = -1;
      }
    }
  }
}

static void eye(const Skin& k, int16_t ex, int16_t ey, uint8_t u, EyeMode m, uint16_t ink) {
  switch (m) {
    case EYE_HAPPY:
      canvas.fillRect(ex, ey + u, u, u, ink);
      canvas.fillRect(ex + u, ey, u, u, ink);
      canvas.fillRect(ex + 2 * u, ey + u, u, u, ink);
      break;
    case EYE_X:
      for (uint8_t i = 0; i < 3; i++) {
        canvas.fillRect(ex + i * u, ey + i * u, u, u, ink);
        canvas.fillRect(ex + i * u, ey + (2 - i) * u, u, u, ink);
      }
      break;
    case EYE_BLINK:
      canvas.fillRect(ex, ey + u, 2 * u, max(1, u / 2), ink);
      break;
    default:
      if (k.eyes == EYES_SLASH) {
        canvas.fillRect(ex + u, ey, u, 2 * u, ink);
        canvas.fillRect(ex, ey + u, u, 2 * u, ink);
      } else {
        canvas.fillRect(ex, ey, 2 * u, 2 * u, ink);
      }
  }
}

// A standing invader, torso top-left at (bx, by). legs: 0 straight, 1 splayed (the second march frame).
// armL/armR: the arm's top row. shimmer: the torso row where a cyan band starts, -1 for none.
static void body(const Skin& k, const Paint& p, int16_t bx, int16_t by, uint8_t u, uint8_t legs,
                 int8_t armL, int8_t armR, EyeMode eyes, int16_t look, int8_t shimmer) {
  int16_t legY = by + 7 * u;
  int16_t lx = bx + k.legL * u, rx = bx + k.legR * u, lw = k.legW * u;
  if (legs) {           // upper half straight, feet kicked outwards
    canvas.fillRect(lx, legY, lw, u, p.body);
    canvas.fillRect(rx, legY, lw, u, p.body);
    canvas.fillRect(lx - u, legY + u, lw, u, p.body);
    canvas.fillRect(rx + u, legY + u, lw, u, p.body);
  } else {
    canvas.fillRect(lx, legY, lw, 2 * u, p.body);
    canvas.fillRect(rx, legY, lw, 2 * u, p.body);
  }
  for (uint8_t r = 0; r < 7; r++) {
    torsoRow(k.rows[r], bx, by + r * u, u, p, shimmer >= 0 && (r == shimmer || r == shimmer + 1));
  }
  if (k.knotHat) knot(bx + 6 * u - 5 * u / 2, by - 4 * u, 5 * u, p.hat);
  if (k.armL >= 0) canvas.fillRect(bx + k.armL * u, by + armL * u, u, 2 * u, p.body);
  if (k.armR >= 0) canvas.fillRect(bx + k.armR * u, by + armR * u, u, 2 * u, p.body);
  int16_t ey = by + k.eyeY * u;
  eye(k, bx + k.eyeL * u + look, ey, u, eyes, p.eye);
  if (k.eyeR >= 0) eye(k, bx + k.eyeR * u + look, ey, u, eyes, p.eye);
}

// Asleep: grey, squashed to torso rows 0/2/4/6, eyes shut, no legs
static void asleep(const Skin& k, int16_t bx, int16_t feet, uint8_t u) {
  Paint p = {GREY, GREY, k.face == INDIGO ? INDIGO : GREY, INDIGO, GREY};
  int16_t by = feet - 4 * u;
  for (uint8_t r = 0; r < 4; r++) torsoRow(k.rows[r * 2], bx, by + r * u, u, p, false);
  if (k.knotHat) knot(bx + 6 * u - 2 * u, by - 4 * u + u / 2, 4 * u, GREY);
  int16_t ey = feet - 3 * u + u / 4;
  uint16_t lid = k.face == INDIGO ? SILVER : INDIGO;
  canvas.fillRect(bx + k.eyeL * u, ey, 2 * u, max(1, u / 2), lid);
  if (k.eyeR >= 0) canvas.fillRect(bx + k.eyeR * u, ey, 2 * u, max(1, u / 2), lid);
}

static Paint paintOf(const char* src, uint8_t color, uint16_t tint) {
  const Skin& k = skinFor(src);
  uint16_t b = strcmp(k.src, src) ? T->pal.fallback[color % 6] : k.body;
  if (tint) {   // an eye in a hole (OC, PI) takes the tint; an eye on the body goes dark
    bool hole = k.face == INDIGO && k.rows[k.eyeY][k.eyeL] == 'F';
    return {tint, tint, k.face == INDIGO ? INDIGO : tint, hole ? tint : INDIGO, tint};
  }
  return {b, b == k.body ? k.accent : b, k.face == k.body ? b : k.face, k.eye, k.accent};
}

static void textAt(const char* s, int16_t x, int16_t y, uint8_t size, uint16_t col) {
  canvas.setTextSize(size);
  canvas.setTextColor(col);
  canvas.setCursor(x, y);
  canvas.print(s);
}

// Where the creature's torso sits after its state's motion, for the reticle and the effects
struct Placed { int16_t bx, by, feet; };

// One agent in one state. [sx, sx+sw) bounds the effects. step 0: no marching (escorts, parade).
static Placed agent(const char* src, uint8_t color, Mood m, unsigned long since, bool alert,
                    int16_t x, int16_t feet, uint8_t u, int16_t step, int16_t dive,
                    int16_t sx, int16_t sw, unsigned long now) {
  const Skin& k = skinFor(src);
  unsigned long t = now - since;
  int16_t dx = 0, dy = 0;
  uint8_t legs = 0;
  int8_t armL = 3, armR = 3, shimmer = -1;
  int16_t look = 0;
  EyeMode eyes = EYE_OPEN;
  uint16_t tint = 0;
  switch (m) {
    case MOOD_SLEEPING:
      asleep(k, x, feet, u);
      {   // a z drifting up and right, every 2.4 s
        uint8_t ts = u >= 6 ? 2 : 1;
        uint16_t zt = now % 2400;
        int16_t zx = min((int)(x + 11 * u + zt / 400 * ts), sx + sw - 6 * ts);
        int16_t zy = feet - 4 * u - 8 * ts - zt * (3 * u) / 2400;
        textAt("z", zx, zy, ts, SILVER);
      }
      return {x, (int16_t)(feet - 4 * u), feet};
    case MOOD_IDLE:
      dx = MARCH[(now / 600) % 8] * step;
      legs = (now / 600) % 2;
      if (now % 4800 < 200) eyes = EYE_BLINK;
      break;
    case MOOD_WORKING:
      dx = MARCH[(now / 150) % 8] * step;
      legs = (now / 150) % 2;
      armL = legs ? 2 : 4;
      armR = legs ? 4 : 2;
      break;
    case MOOD_THINKING: {
      static const int8_t LOOK[4] = {0, -1, 1, 0};
      look = LOOK[(now / 600) % 4] * max(1, u / 2);
      shimmer = (now / 100) % 8 - 1;   // a band sweeping down the torso, then a beat of rest
      break;
    }
    case MOOD_WAITING:
      // Turns yellow and drops toward the ship; an alert one flashes and kicks
      tint = !alert || (now / 200) % 3 != 2 ? YELLOW : 0;
      if (alert) {
        dy = dive;
        legs = (now / 200) % 2;
      }
      armL = armR = 1;
      break;
    case MOOD_DONE:
      tint = t < 1200 && (now / 150) % 2 ? WHITE : 0;
      dy = t < 1500 ? -((now / 150) % 2) * max(2, (int)u) : 0;
      armL = armR = 1;
      eyes = EYE_HAPPY;
      break;
    case MOOD_ERROR: {
      static const int8_t SHAKE[4] = {0, 1, -1, 1};
      tint = !alert || (now / 200) % 2 ? RED : 0;
      dx = SHAKE[(now / 100) % 4] * (u / 3 + 1);
      eyes = EYE_X;
      break;
    }
    default: break;
  }
  Paint p = paintOf(src, color, tint);
  int16_t bx = x + dx, by = feet - 9 * u + dy;
  body(k, p, bx, by, u, legs, armL, armR, eyes, look, shimmer);

  int16_t cx = bx + 6 * u, cy = by + 4 * u;
  if (m == MOOD_ERROR) {   // an explosion: 8 rays of two cells flying out, every 800 ms
    uint8_t phase = (now % 800) / 100;
    if (phase < 6) {
      int16_t ps = max(2, u / 2);
      int16_t r = min(4 * u + phase * u / 2, max((int)u, min(sw / 2 - 2 * u - 2 * ps, 11 * u / 2)));
      for (int8_t i = 0; i < 8; i++) {
        static const int8_t UX[8] = {1, 1, 0, -1, -1, -1, 0, 1}, UY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
        for (uint8_t j = 0; j < 2; j++) {
          if (j == 0 && phase >= 3) continue;   // the inner cell burns out first
          int16_t px = cx + UX[i] * (r + 2 * u + j * ps), py = cy + UY[i] * (r + j * ps);
          canvas.fillRect(px - ps / 2, py - ps / 2, ps, ps, j ? RED : YELLOW);
        }
      }
    }
  }
  if (m == MOOD_DONE && t < 1500) {   // "+1" pops up and floats away
    uint8_t ts = u >= 5 ? 2 : 1;
    // Beside a knot hat rather than inside it, where it would be white on white
    int16_t px = k.knotHat ? min(cx + 5 * u - 5 * u / 2 + 1, sx + sw - 12 * ts) : cx - 6 * ts + 1;
    textAt("+1", px, by - 8 * ts - (int16_t)(t / 60) * ts, ts, WHITE);
  }
  if (m == MOOD_WAITING && u >= 5) {   // room for a question mark beside the head
    const uint8_t* q = I_ASK;
    uint8_t iu = u / 4;
    int16_t qx = min((int)(bx + 12 * u + 5), sx + sw - 7 * iu - 1), qy = by - 7 * iu - 2;
    if ((now / 300) % 2 || !alert) {
      for (uint8_t r = 0; r < 7; r++) {
        for (uint8_t c = 0; c < 7; c++) {
          if (q[r] & (0x40 >> c)) canvas.fillRect(qx + c * iu, qy + r * iu, iu, iu, YELLOW);
        }
      }
    }
  }
  return {bx, by, (int16_t)(by + 9 * u)};
}

// Sub-agents: at most three tiny copies hovering above, then "+N". Each bobs a beat behind the last.
static void escorts(const Session& s, int16_t cx, int16_t top, const Slot& sl, unsigned long now) {
  uint8_t n = s.followerCount;
  if (!n) return;
  uint8_t fu = sl.fu, shown = min((int)n, 3), extra = n - shown, ts = fu >= 2 ? 2 : 1;
  int16_t pitch = 14 * fu;
  int16_t badge = extra ? 6 * ts * (extra >= 10 ? 3 : 2) + 2 : 0;
  int16_t width = shown * pitch - 2 * fu + badge;
  int16_t left = max((int)sl.x + 1, min(cx - width / 2, sl.x + sl.w - 1 - width));
  int16_t feet = top - 2;
  // Dropping in, leaving and bobbing never lift an escort over the HUD. Lower rows may already rest
  // under the row above, so there they only stop the motion from going any higher.
  int16_t lowest = sl.ceiling + 9 * fu + (skinFor(s.src).knotHat ? 4 * fu : 0);
  if (sl.row) lowest = min(lowest, feet);
  for (uint8_t i = 0; i < shown; i++) {
    const Follower& f = s.followers[i];
    unsigned long age = now - f.bornMs, beat = now + i * 150;
    int16_t y = feet - (age < DROP_MS ? (int16_t)((DROP_MS - age) / 20) : 0);
    int16_t hop = 0, dx = 0;
    Mood m = f.mood;
    unsigned long t = now - f.moodSinceMs;
    if (f.leaving) {                    // done: white, rising, sparkling
      y -= t / 100 * fu;
      m = MOOD_DONE;
    } else if (m == MOOD_WAITING) {
      hop = (beat / 150) % 2 * 2 * fu;
    } else if (m != MOOD_THINKING) {
      m = MOOD_WORKING;
      hop = (beat / 300) % 2 * fu;
    }
    y = max(y, lowest);
    // With no headroom (three rows, or a knot hat under the HUD) the hop becomes a sideways shuffle,
    // so a working or waiting escort still moves.
    if (y - hop >= lowest) y -= hop;
    else if (hop) dx = fu;
    if (f.leaving) {
      for (uint8_t j = 0; j < 4; j++) {
        int16_t r = 3 * fu + (t / 100) % 4 * fu;
        int16_t ex = left + i * pitch + 6 * fu + (j % 2 ? r : -r), ey = y - 5 * fu + (j / 2 ? r : -r);
        if (ey >= sl.ceiling) canvas.fillRect(ex, ey, fu, fu, T->pal.confetti[(j + t / 100) % 6]);
      }
    }
    agent(s.src, s.color, m, f.leaving ? f.moodSinceMs : now - 2000, m == MOOD_WAITING,
          left + i * pitch + dx, y, fu, 0, 0, sl.x, sl.w, beat);
  }
  if (extra) {
    char buf[5];
    snprintf(buf, sizeof(buf), "+%u", extra);
    textAt(buf, left + shown * pitch, feet - 7 * ts, ts, SILVER);
  }
}

// ── Sky, HUD, ship, marquee ─────────────────────────────────────

static const struct { uint8_t x, y, phase; } STARS[] = {
  {12, 30, 0}, {47, 24, 5}, {83, 41, 2}, {118, 27, 7}, {157, 36, 3}, {196, 25, 6}, {228, 44, 1},
  {28, 66, 4}, {66, 88, 1}, {104, 62, 6}, {141, 79, 0}, {179, 70, 5}, {215, 92, 2},
  {9, 112, 7}, {52, 128, 3}, {93, 106, 4}, {170, 118, 1}, {231, 131, 6},
  {22, 158, 2}, {75, 170, 5}, {130, 150, 7}, {201, 164, 0},
};

static void sky(unsigned long now) {
  canvas.fillRect(0, 0, DISP_W, DISP_H, INDIGO);
  for (const auto& s : STARS) {
    uint8_t beat = (now / 300 + s.phase) % 8;   // 2.4 s
    canvas.writePixel(s.x, s.y, beat == 0 ? WHITE : NIGHT);
    if (beat == 0) {
      canvas.drawFastHLine(s.x - 1, s.y, 3, WHITE);
      canvas.drawFastVLine(s.x, s.y - 1, 3, WHITE);
    }
  }
  canvas.fillRect(0, GROUND_Y, DISP_W, 2, MAGENTA);
}

// The laser cannon, 13x7 cells of 2 px, centred on cx
static void ship(int16_t cx) {
  static const uint16_t ROWS[7] = {0x0040, 0x00E0, 0x00E0, 0x0FFE, 0x1FFF, 0x1FFF, 0x1FFF};
  int16_t x = cx - 13;
  for (uint8_t r = 0; r < 7; r++) {
    int16_t run = -1;
    for (int8_t c = 0; c <= 13; c++) {
      bool on = c < 13 && (ROWS[r] & (1 << (12 - c)));
      if (on && run < 0) run = c;
      if (!on && run >= 0) {
        canvas.fillRect(x + run * 2, SHIP_Y + r * 2, (c - run) * 2, 2, r == 0 ? WHITE : CYAN);
        run = -1;
      }
    }
  }
}

static void bracket(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t len, int16_t th, uint16_t col) {
  canvas.fillRect(x0, y0, len, th, col);          canvas.fillRect(x0, y0, th, len, col);
  canvas.fillRect(x1 - len + 1, y0, len, th, col); canvas.fillRect(x1 - th + 1, y0, th, len, col);
  canvas.fillRect(x0, y1 - th + 1, len, th, col);  canvas.fillRect(x0, y1 - len + 1, th, len, col);
  canvas.fillRect(x1 - len + 1, y1 - th + 1, len, th, col);
  canvas.fillRect(x1 - th + 1, y1 - len + 1, th, len, col);
}

static const char* const WORDS[7] = {"空闲", "思考中", "工作中", "等你批准", "完成", "出错", "睡眠中"};

static void hud(uint8_t n, unsigned long now) {
  char buf[12];
  textAt("SQUAD", 4, HUD_Y, 2, SILVER);
  snprintf(buf, sizeof(buf), "%02u", n);
  textAt(buf, 76, HUD_Y, 2, WHITE);
  uint8_t waits = 0, errors = 0;
  bool alert = false;
  for (uint8_t i = 0; i < n; i++) {
    waits += sessions[i].mood == MOOD_WAITING;
    errors += sessions[i].mood == MOOD_ERROR;
    alert |= isAlerting(sessions[i]);
  }
  bool on = !alert || (now / 300) % 2 == 0;
  uint16_t col = CYAN;
  if (waits) snprintf(buf, sizeof(buf), "WAIT %u", waits), col = YELLOW;
  else if (errors) snprintf(buf, sizeof(buf), "ERR %u", errors), col = RED;
  else strlcpy(buf, "READY", sizeof(buf));
  if (on) textAt(buf, DISP_W - 2 - 12 * strlen(buf), HUD_Y, 2, col);
}

static void marquee(const Session& s, uint8_t focus, uint8_t n, unsigned long now) {
  Mood m = s.mood < MOOD_UNKNOWN ? s.mood : MOOD_IDLE;
  const char* word = WORDS[m];
  const uint8_t* icon = m == MOOD_IDLE ? I_IDLE : iconFor(m, now);
  int16_t wordW = labelFont.getUTF8Width(word);
  const int16_t iconW = 14, gap = 6;
  char title[sizeof(s.title)];
  int16_t titleW = fitLabel(title, sizeof(title), labelOf(s), DISP_W - 8 - iconW - 2 * gap - wordW);
  int16_t x = (DISP_W - (iconW + gap + titleW + gap + wordW)) / 2;
  drawIcon(x, TITLE_BASE - 12, icon, 2, stateColor(m));
  labelFont.setForegroundColor(WHITE);
  labelFont.drawUTF8(x + iconW + gap, TITLE_BASE, title);
  labelFont.setForegroundColor(stateColor(m));
  labelFont.drawUTF8(x + iconW + gap + titleW + gap, TITLE_BASE, word);

  if (n < 2) return;   // one pip per invader, the focused one framed
  const int16_t pw = 8, pg = 5;
  int16_t px = (DISP_W - n * (pw + pg) + pg) / 2;
  for (uint8_t i = 0; i < n; i++, px += pw + pg) {
    const Session& o = sessions[i];
    if (!isAlerting(o) || (now / 300) % 2 == 0) canvas.fillRect(px, PIPS_Y, pw, 5, stateColor(o.mood));
    if (i == focus) canvas.drawRect(px - 2, PIPS_Y - 2, pw + 4, 9, WHITE);
  }
}

// ── Hooks ───────────────────────────────────────────────────────

static int16_t shipX = -1;          // the ship's centre; it slides, so it is the one piece of state
static unsigned long shipMs = 0;

static void enter(unsigned long) { shipX = -1; }

static uint8_t focusOf(uint8_t n, unsigned long now) {
  int8_t held = heldUrgent(now);
  return held >= 0 && held < n ? held : (now / FOCUS_MS) % n;
}

static void page(uint8_t first, uint8_t count, int16_t, int16_t, unsigned long now) {
  sky(now);
  uint8_t n = count;
  hud(n, now);
  if (!n) return;
  uint8_t focus = focusOf(n, now);
  Placed focused = {0, 0, 0};
  Slot fs = slotOf(focus, n);
  for (uint8_t i = 0; i < n; i++) {
    const Session& s = sessions[first + i];
    Slot sl = slotOf(i, n);
    int16_t x = sl.x + (sl.w - 12 * sl.u) / 2;
    Placed p = agent(s.src, s.color, s.mood, s.moodSinceMs, isAlerting(s), x, sl.feet, sl.u, sl.step, sl.dive,
                     sl.x, sl.w, now);
    const Skin& k = skinFor(s.src);
    int16_t top = min(p.by, (int16_t)(sl.feet - 9 * sl.u)) - (k.knotHat ? 4 * sl.u : 0);
    escorts(s, p.bx + 6 * sl.u, top, sl, now);
    if (i == focus) focused = p;
  }
  // Reticle on the focused invader, and the ship sliding under it
  uint8_t u = fs.u;
  int16_t pad = u >= 5 ? 4 : 2;
  bracket(focused.bx - pad, focused.feet - 9 * u - pad, focused.bx + 12 * u - 1 + pad, focused.feet + pad - 1,
          max(3, u + 1), u >= 5 ? 2 : 1, CYAN);
  int16_t target = fs.x + fs.w / 2;
  long dt = (long)(now - shipMs);
  if (shipX < 0 || dt > 1000 || dt <= 0) {
    shipX = target;
  } else if (shipX != target) {
    int16_t d = target - shipX;
    int16_t mv = max((int)(abs(d) * dt / 200), (int)(6 * dt / 100));
    shipX = abs(d) <= mv ? target : shipX + (d > 0 ? mv : -mv);
  }
  shipMs = now;
  ship(shipX);
  marquee(sessions[first + focus], focus, n, now);
}

// Attract mode: the title, a slow parade of the whole cast, a blinking call to plug an agent in
static void attract(unsigned long now) {
  sky(now);
  textAt("CLAWD", 77, 32, 3, NIGHT);
  textAt("CLAWD", 75, 30, 3, MAGENTA);
  textAt("INVADERS", 52, 62, 3, NIGHT);
  textAt("INVADERS", 50, 60, 3, CYAN);
  const uint8_t u = 3;
  const int16_t spacing = 48, feet = 146;
  int16_t shift = (now / 200) % 48 * SKIN_COUNT;   // SKIN_COUNT px a step: the whole cast, one spacing each, in 9.6 s
  uint8_t legs = (now / 200) % 2;
  for (uint8_t i = 0; i < SKIN_COUNT; i++) {
    const Skin& k = T->skins[i];
    int16_t x = (i * spacing + shift) % (SKIN_COUNT * spacing) - spacing + 6;
    Paint p = paintOf(k.src, 0, 0);
    body(k, p, x, feet - 9 * u, u, legs, legs ? 2 : 4, legs ? 4 : 2, EYE_OPEN, 0, -1);
  }
  if ((now / 600) % 2 == 0) textAt("INSERT AGENT", 49, 158, 2, YELLOW);
  ship(DISP_W / 2);
  drawLabelCentered("等待智能体连接", TITLE_BASE, SILVER);
}

// The settings confirmation as an arcade banner across the middle
static void toast(const char* text, unsigned long now) {
  const int16_t y = 98, h = 36;
  canvas.fillRect(0, y, DISP_W, h, INDIGO);
  canvas.fillRect(0, y, DISP_W, 2, MAGENTA);
  canvas.fillRect(0, y + h - 2, DISP_W, 2, MAGENTA);
  for (int16_t x = 2; x < DISP_W; x += 8) {
    canvas.fillRect(x, y + 4, 4, 2, NIGHT);
    canvas.fillRect(x, y + h - 6, 4, 2, NIGHT);
  }
  drawLabelCentered(text, y + 23, YELLOW);
  int16_t room = (DISP_W - labelFont.getUTF8Width(text)) / 2;
  if (room >= 20) {   // a small Clawd marching at each end, when the text leaves room
    const Skin& k = T->skins[0];
    Paint p = paintOf(k.src, 0, 0);
    uint8_t legs = (now / 300) % 2;
    body(k, p, room / 2 - 6, y + 11, 1, legs, 3, 3, EYE_OPEN, 0, -1);
    body(k, p, DISP_W - room / 2 - 6, y + 11, 1, !legs, 3, 3, EYE_OPEN, 0, -1);
  }
}

const Theme THEME_ARCADE = {
  4, "arcade", "街机", "小队变成入侵者阵列", 0xFF3EA5, 2, MAX_SESSIONS, 0,
  {
    INDIGO, INDIGO, INDIGO, RED,           // bg gap waitFlash (the sky never flashes) errorFrame
    INDIGO, NIGHT, WHITE, SILVER, SILVER,  // plate tileBorder text tag more
    INDIGO, CYAN, GREY,                    // pagerBg pagerOn pagerOff (one page: never drawn)
    GREY, INDIGO,                          // smoke sleepTarget
    INDIGO, YELLOW,                        // toastBg toastText
    {SILVER, CYAN, LIME, YELLOW, WHITE, RED, GREY},
    {MAGENTA, LIME, CYAN, VIOLET, TEAL, SILVER},
    {MAGENTA, CYAN, YELLOW, WHITE, LIME, ORANGE},
  },
  SKINS, KNOT_LOGO,
  {I_GEAR_A, I_GEAR_B, I_KEYS_A, I_KEYS_B, I_ASK, I_CHECK, I_CROSS, I_ZZZ},
  {nullptr, nullptr, attract, nullptr, page, nullptr, nullptr, nullptr, toast, enter},
  {{12000, 9600, 10}, {0, 4800, 10}, {0, 9600, 10}},
};
