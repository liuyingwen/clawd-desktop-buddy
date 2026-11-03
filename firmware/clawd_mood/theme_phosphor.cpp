// Theme 3, 荧光终端: a green phosphor terminal running a process table, one row per session,
// 6 rows per page. Pages scroll up a row at a time like terminal output. One session gets a big
// creature over a short log; nobody connected types a boot log.
// Everything is one green at three levels; only waiting (amber) and error (red) leave it.
// Animations repeat every 300, 600 or 1200 ms, so all three previews loop in their scene's length.
#include "theme.h"

// RGB565 values picked so dimColor (halfway to BG, as sleeping creatures are drawn) of each green
// lands exactly on the next one down: HI -> MID -> LO, and BG -> BG. A sleeping row adds no colour.
#define BG       RGB(0, 24, 16)      // 565 (0, 6, 2)
#define SCAN     RGB(0, 8, 0)        // every other row of the background
#define BLACK    0x0000              // tube corners, text on amber and on the toast
#define HI       RGB(48, 252, 112)   // (6, 63, 14)
#define MID      RGB(24, 136, 64)    // (3, 34, 8) = dimColor(HI)
#define LO       RGB(8, 80, 40)      // (1, 20, 5) = dimColor(MID)
#define PALE     RGB(168, 255, 200)  // done, and every other unknown agent
#define AMBER    RGB(255, 176, 0)
#define AMBER_DK RGB(120, 80, 0)
#define RED      RGB(255, 64, 64)
#define RED_DK   RGB(120, 24, 24)

#define STATUS_H 16                  // top status line
#define ROW_H    34
#define ROWS     6
#define BAR_Y    (STATUS_H + ROWS * ROW_H)   // 220: prompt line, or the pager when there are pages
#define BAR_H    (DISP_H - BAR_Y)
#define DROP_MS  400UL               // a new follower falls into line, as the built-in ones do

static const uint8_t I_GEAR_A[7]  = {0x08,0x49,0x2A,0x63,0x2A,0x49,0x08};
static const uint8_t I_GEAR_B[7]  = {0x22,0x14,0x22,0x41,0x22,0x14,0x22};
static const uint8_t I_KEYS_A[7]  = {0x00,0x55,0x00,0x2A,0x00,0x7F,0x00};
static const uint8_t I_KEYS_B[7]  = {0x00,0x2A,0x00,0x55,0x00,0x7F,0x00};
static const uint8_t I_ASK[7]     = {0x1C,0x22,0x02,0x0C,0x08,0x00,0x08};
static const uint8_t I_CHECK[7]   = {0x00,0x01,0x02,0x44,0x28,0x10,0x00};
static const uint8_t I_CROSS[7]   = {0x41,0x22,0x14,0x08,0x14,0x22,0x41};
static const uint8_t I_ZZZ[7]     = {0x7C,0x08,0x10,0x20,0x7C,0x00,0x00};

// Monochrome: the agents are told apart by silhouette alone, and by the tag beside them.
// Solid ones carry dark eyes; hollow ones (OC, PI's counter) show the screen through the face.
static const Skin SKINS[] = {
  {"CC", {".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBBB."},
   HI, 0, 0, BG, 0, 11, 2, 8, 2, 3, 7, 2, EYES_SQUARE},
  {"CX", {"..BBBBBBBB..",      // solid robot under the knot
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          "..BBBBBBBB.."},
   HI, 0, HI, BG, 0, 11, 3, 7, 2, 3, 7, 2, EYES_SQUARE, true},
  {"OC", {".BBBBBBBBBB.",      // hollow frame
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BBBBBBBBBB."},
   HI, 0, BG, HI, 0, 11, 3, 7, 2, 3, 7, 2, EYES_SQUARE},
  {"PI", {"BBBBBBBBB...",      // P whose counter holds one eye
          "BBBFFFFBB...",
          "BBBFFFFBB...",
          "BBBFFFFBB...",
          "BBBBBB...BBB",
          "BBBBBB...BBB",
          "BBB......BBB"},
   HI, 0, BG, HI, -1, -1, 0, 9, 3, 4, -1, 1, EYES_SQUARE},
  {"DS", {"..........BB",      // whale, tail up, lighter belly
          "..BBBBB..BB.",
          ".BBBBBBB.BB.",
          ".BBBBBBBBBB.",
          ".BBBBBBBBB..",
          ".AAAAAAA....",
          "..AAAAAA...."},
   HI, MID, 0, BG, 0, -1, 2, 6, 2, 2, -1, 2, EYES_SQUARE},
  {"WB", {".B........B.",      // cat, ears up, darker face
          ".BB......BB.",
          ".BBBBBBBBBB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BFFFFFFFFB.",
          ".BBBBBBBBBB."},
   HI, 0, MID, BG, 0, 11, 3, 7, 2, 3, 7, 3, EYES_SLASH},
  {"CU", {"....AAAA....",      // isometric cube: dim top over two lit sides
          "..AAAAAAAA..",
          ".BAAAAAAAAF.",
          ".BBBAAAAFFF.",
          ".BBBBBFFFFF.",
          ".BBBBBFFFFF.",
          "...BBBFFF..."},
   HI, MID, HI, BG, 0, 11, 3, 7, 2, 3, 7, 4, EYES_SQUARE},
};
static_assert(sizeof(SKINS) / sizeof(SKINS[0]) == SKIN_COUNT, "every theme carries all agents");

// Inks: a creature is drawn in green, then its rectangle is recoloured. Small rects only.
typedef uint16_t InkPair[2];
static const InkPair INK_AMBER_ON[] = {{HI, BLACK}, {PALE, BLACK}, {MID, AMBER_DK}, {LO, AMBER_DK}, {BG, AMBER}, {SCAN, AMBER}};
static const InkPair INK_AMBER[]    = {{HI, AMBER}, {PALE, AMBER}, {MID, AMBER_DK}, {LO, AMBER_DK}};
static const InkPair INK_RED[]      = {{HI, RED}, {PALE, RED}, {MID, RED_DK}, {LO, RED_DK}};
#define INK(a) a, (uint8_t)(sizeof(a) / sizeof(a[0]))

static void recolor(int16_t x, int16_t y, int16_t w, int16_t h, const InkPair* ink, uint8_t n) {
  if (x < 0) w += x, x = 0;
  if (y < 0) h += y, y = 0;
  if (x + w > DISP_W) w = DISP_W - x;
  if (y + h > DISP_H) h = DISP_H - y;
  if (w <= 0 || h <= 0) return;
  uint16_t* row = canvas.getBuffer() + y * DISP_W + x;
  for (int16_t r = 0; r < h; r++, row += DISP_W) {
    for (int16_t c = 0; c < w; c++) {
      for (uint8_t k = 0; k < n; k++) {
        if (row[c] == ink[k][0]) { row[c] = ink[k][1]; break; }
      }
    }
  }
}

// ── Pieces ──────────────────────────────────────────────────────

static bool blink(unsigned long now, unsigned long period) { return (now % period) < period / 2; }

// Near-black green with a darker line on every other screen row
static void background(int16_t ox, int16_t y, int16_t h) {
  canvas.fillRect(ox, y, DISP_W, h, BG);
  for (int16_t r = y | 1; r < y + h; r += 2) canvas.drawFastHLine(ox, r, DISP_W, SCAN);
}

// The tube's rounded corners
static const uint8_t CORNER[] = {5, 3, 2, 1, 1};
static void corners(bool top, bool bottom) {
  for (uint8_t i = 0; i < sizeof(CORNER); i++) {
    if (top) {
      canvas.drawFastHLine(0, i, CORNER[i], BLACK);
      canvas.drawFastHLine(DISP_W - CORNER[i], i, CORNER[i], BLACK);
    }
    if (bottom) {
      canvas.drawFastHLine(0, DISP_H - 1 - i, CORNER[i], BLACK);
      canvas.drawFastHLine(DISP_W - CORNER[i], DISP_H - 1 - i, CORNER[i], BLACK);
    }
  }
}

static int16_t text(const char* s, int16_t x, int16_t baseline, uint16_t col) {
  labelFont.setForegroundColor(col);
  labelFont.drawUTF8(x, baseline, s);
  return labelFont.getUTF8Width(s);
}

static void small(const char* s, int16_t x, int16_t y, uint16_t col, uint8_t size = 1) {
  canvas.setTextSize(size);
  canvas.setTextColor(col);
  canvas.setCursor(x, y);
  canvas.print(s);
  canvas.setTextSize(1);
}

static void cursorBlock(int16_t x, int16_t baseline, uint16_t col, unsigned long now) {
  if (blink(now, 600)) canvas.fillRect(x, baseline - 10, 6, 12, col);
}

static bool anyAlerting(Mood m) {
  for (uint8_t i = 0; i < sessionCount; i++) {
    if (isAlerting(sessions[i]) && sessions[i].mood == m) return true;
  }
  return false;
}

// "clawd@desk:~$ squad", a cursor, and the session count; a blinking mark while anyone needs you
static void statusLine(int16_t ox, unsigned long now) {
  int16_t x = ox + 7;
  x += text("clawd@desk:~$ ", x, 12, MID);
  x += text("squad", x, 12, HI);
  cursorBlock(x + 2, 12, HI, now);
  char n[12];
  snprintf(n, sizeof(n), "%u 会话", sessionCount);
  int16_t w = labelFont.getUTF8Width(n);
  int16_t right = ox + DISP_W - 7 - w;
  text(n, right, 12, HI);
  uint16_t alarm = anyAlerting(MOOD_WAITING) ? AMBER : anyAlerting(MOOD_ERROR) ? RED : 0;
  if (alarm && blink(now, 600)) {
    canvas.fillRect(right - 11, 2, 7, 11, alarm);
    canvas.fillRect(right - 8, 4, 1, 5, BLACK);
    canvas.fillRect(right - 8, 10, 1, 1, BLACK);
  }
  canvas.drawFastHLine(ox, STATUS_H - 1, DISP_W, LO);
}

static void promptLine(int16_t ox, int16_t y, unsigned long now) {
  canvas.drawFastHLine(ox, y, DISP_W, LO);
  int16_t x = ox + 7;
  x += text("$ ", x, y + 15, MID);
  cursorBlock(x, y + 15, HI, now);
}

// Followers in a row from x, at most maxW wide, "+N" for the rest. tint: the waiting one in amber.
// ceiling: the text above them, which a dropping follower must not cover.
static void followers(const Session& s, int16_t x, int16_t maxW, int16_t feetY, int16_t ceiling, uint8_t fu,
                      bool tint, uint16_t more, unsigned long now) {
  uint8_t n = s.followerCount;
  if (!n) return;
  const int16_t pitch = 12 * fu + 2, badge = 20;
  const int16_t height = skinFor(s.src).knotHat ? 14 * fu - fu / 2 : 9 * fu;
  const int16_t room = max(0, feetY - height - ceiling);
  uint8_t shown = n * pitch <= maxW ? n : maxW > badge ? (maxW - badge) / pitch : 0;
  for (uint8_t i = 0; i < shown; i++) {
    const Follower& f = s.followers[i];
    unsigned long age = now - f.bornMs;
    int16_t drop = age < DROP_MS ? min((int)room, (int)((DROP_MS - age) / 100 * fu)) : 0;
    drawCreature(s.src, s.color, f.mood, f.moodSinceMs, tint && f.mood == MOOD_WAITING,
                 x + i * pitch, feetY - drop, fu, now + i * 110);
  }
  if (shown < n) {
    char buf[6];
    snprintf(buf, sizeof(buf), "+%u", n - shown);
    small(buf, x + shown * pitch + 2, feetY - 8 * fu / 2 - 4, more);
  }
}

static const char* const TAGS[7] = {"[空闲]", "[思考]", "[工作]", "[等你批准]", "[完成]", "[出错]", "[睡眠]"};
static const uint8_t MARK_W[7] = {9, 10, 34, 10, 11, 22, 13};

// Each state's own motion: _ blinks, |/-\ spins, a bar marches, ? stands, a check, an ERR box,
// z's rising. (x, y) is the top-left of a MARK_W[m] x 13 slot.
static void stateMark(Mood m, int16_t x, int16_t y, uint16_t col, uint16_t dim, unsigned long now) {
  switch (m) {
    case MOOD_IDLE:
      if (blink(now, 1200)) canvas.fillRect(x, y + 10, 9, 2, col);
      break;
    case MOOD_THINKING: {
      static const char SPIN[] = "|/-\\";
      char c[2] = {SPIN[(now / 150) % 4], 0};
      small(c, x, y - 1, col, 2);
      break;
    }
    case MOOD_WORKING:   // decorative: a pattern marching right, it measures nothing
      for (uint8_t i = 0; i < 6; i++) {
        bool lit = (i + 3 - (now / 200) % 3) % 3 != 0;
        canvas.fillRect(x + i * 6, y + 3, 4, 8, lit ? col : dim);
      }
      break;
    case MOOD_WAITING:
      small("?", x, y - 1, col, 2);
      break;
    case MOOD_DONE:
      for (int8_t k = 0; k < 2; k++) {
        canvas.drawLine(x, y + 6 + k, x + 3, y + 9 + k, col);
        canvas.drawLine(x + 3, y + 9 + k, x + 10, y + 2 + k, col);
      }
      break;
    case MOOD_ERROR:
      if (blink(now, 600)) {
        canvas.fillRect(x, y + 1, 22, 11, col);
        small("ERR", x + 2, y + 3, BLACK);
      } else {
        canvas.drawRect(x, y + 1, 22, 11, col);
        small("ERR", x + 2, y + 3, col);
      }
      break;
    case MOOD_SLEEPING: {
      uint8_t p = (now / 400) % 3;
      small("z", x, y + 6 - p * 2, col);
      if (p) small("z", x + 7, y + 2 - (p - 1) * 2, col);
      break;
    }
    default:
      break;
  }
}

// ── The process table ───────────────────────────────────────────

// Row geometry. Compact fits 6 rows; up to 4 sessions on a single page get roomy rows that
// fill the same table with bigger creatures. All offsets are from the row's top.
struct RowLayout {
  int16_t h;
  uint8_t u, fu;          // creature and follower units
  int16_t left;           // text column
  int16_t titleBase, tagTop, nameBase, followerFeet, markTop, creatureFeet;
};
static const RowLayout COMPACT = {ROW_H, 2, 1, 38, 13, 21, 29, 34, 19, 32};
static const RowLayout ROOMY = {ROWS * ROW_H / 4, 3, 2, 50, 19, 31, 39, 49, 28, 47};

static const RowLayout& rowLayout() { return sessionCount <= 4 ? ROOMY : COMPACT; }

// One session, rows [y, y+L.h): creature, title, state tag; under the title its agent tag and
// its followers, or the directory; the state's mark under the tag.
static void drawRow(const Session& s, const RowLayout& L, int16_t ox, int16_t y, unsigned long now) {
  Mood m = s.mood < MOOD_UNKNOWN ? s.mood : MOOD_IDLE;
  bool alert = isAlerting(s);
  // Waiting alternates inverse amber with an amber outline; a quiet one holds the outline
  bool inverse = m == MOOD_WAITING && alert && blink(now, 600);
  uint16_t title = HI, tag = m == MOOD_IDLE ? MID : HI, sub = MID, dim = LO;
  switch (m) {
    case MOOD_SLEEPING: title = MID; tag = MID; sub = LO; break;
    case MOOD_DONE:
      canvas.fillRect(ox, y + 1, DISP_W, L.h - 2, LO);
      title = tag = PALE; sub = HI; dim = MID;
      break;
    case MOOD_WAITING:
      if (inverse) {
        canvas.fillRect(ox, y + 1, DISP_W, L.h - 2, AMBER);
        title = tag = sub = BLACK; dim = AMBER_DK;
      } else {
        canvas.drawRect(ox + 1, y + 1, DISP_W - 2, L.h - 2, AMBER);
        title = tag = sub = AMBER; dim = AMBER_DK;
      }
      break;
    case MOOD_ERROR: title = tag = sub = RED; dim = RED_DK; break;
    default: break;
  }

  const int16_t spriteW = L.left - 6;
  drawCreature(s.src, s.color, m, s.moodSinceMs, false, ox + (spriteW - 12 * L.u) / 2 + 2, y + L.creatureFeet, L.u, now);
  if (inverse) recolor(ox + 2, y + 2, spriteW, L.h - 4, INK(INK_AMBER_ON));
  else if (m == MOOD_WAITING) recolor(ox + 2, y + 2, spriteW, L.h - 4, INK(INK_AMBER));
  else if (m == MOOD_ERROR) recolor(ox + 2, y + 2, spriteW, L.h - 4, INK(INK_RED));

  const int16_t left = ox + L.left, right = ox + DISP_W - 6;
  int16_t tagW = labelFont.getUTF8Width(TAGS[m]);
  text(TAGS[m], right - tagW, y + L.titleBase, tag);
  drawLabel(labelOf(s), left, y + L.titleBase, right - tagW - 6 - left, title);

  small(s.src[0] ? s.src : "--", left, y + L.tagTop, sub);
  const int16_t markX = right - 36;
  if (s.followerCount) {
    // ├ from the agent tag to its followers
    uint16_t branch = dim == LO ? MID : dim;
    int16_t top = y + L.titleBase + 4, mid = y + L.tagTop + 3;
    canvas.drawFastVLine(left + 15, top, mid - top + 4, branch);
    canvas.drawFastHLine(left + 15, mid, 5, branch);
    int16_t fx = left + 22, fw = markX - 4 - fx;
    followers(s, fx, fw, y + L.followerFeet, y + L.titleBase + 2, L.fu, !inverse, sub, now);
    if (inverse) recolor(fx, top, fw, y + L.h - 1 - top, INK(INK_AMBER_ON));
    else if (m == MOOD_ERROR) recolor(fx, top, fw, y + L.h - 1 - top, INK(INK_RED));
  } else if (s.title[0]) {
    drawLabel(s.name, left + 16, y + L.nameBase, markX - 4 - left - 16, sub);
  }
  stateMark(m, right - MARK_W[m], y + L.markTop, m == MOOD_IDLE ? HI : tag, dim, now);
}

// ── One session: a big creature over a short log ────────────────

static const char* command(const char* src) {
  static const char* const CMD[SKIN_COUNT][2] = {
    {"CC", "claude"}, {"CX", "codex"}, {"OC", "opencode"}, {"PI", "pi"}, {"DS", "dsh"}, {"WB", "workbuddy"}, {"CU", "cursor"}};
  for (const auto& c : CMD) {
    if (!strcmp(c[0], src)) return c[1];
  }
  return "agent";
}

static const char* const PHRASES[7] = {"空闲", "思考中", "正在工作", "等你批准", "完成", "出错了", "睡眠中"};

static void solo(const Session& s, int16_t ox, unsigned long now) {
  Mood m = s.mood < MOOD_UNKNOWN ? s.mood : MOOD_IDLE;
  const uint8_t u = 8;
  const int16_t cx = ox + (DISP_W - 12 * u) / 2, feet = 136;
  drawCreature(s.src, s.color, m, s.moodSinceMs, false, cx, feet, u, now);
  if (m == MOOD_WAITING) recolor(cx - u, STATUS_H, 14 * u, feet - STATUS_H, INK(INK_AMBER));
  if (m == MOOD_ERROR) recolor(cx - u, STATUS_H, 14 * u, feet - STATUS_H, INK(INK_RED));
  for (int16_t x = ox + 12; x < ox + DISP_W - 12; x += 6) canvas.drawFastHLine(x, feet + 3, 3, LO);

  // $ claude 固件主题
  int16_t x = ox + 8;
  x += text("$ ", x, 160, MID);
  x += text(command(s.src), x, 160, MID);
  drawLabel(labelOf(s), x + 6, 160, ox + DISP_W - 8 - x - 6, HI);

  // > 正在工作 and the state's mark
  bool alert = isAlerting(s);
  bool inverse = m == MOOD_WAITING && alert && blink(now, 600);
  uint16_t ink = m == MOOD_WAITING ? AMBER : m == MOOD_ERROR ? RED : m == MOOD_SLEEPING ? MID : m == MOOD_DONE ? PALE : HI;
  uint16_t dim = m == MOOD_WAITING ? AMBER_DK : m == MOOD_ERROR ? RED_DK : LO;
  if (inverse) {
    canvas.fillRect(ox + 4, 167, DISP_W - 8, 19, AMBER);
    ink = BLACK;
  } else if (m == MOOD_WAITING) {
    canvas.drawRect(ox + 4, 167, DISP_W - 8, 19, AMBER);
  }
  x = ox + 8;
  x += text("> ", x, 181, ink);
  x += text(PHRASES[m], x, 181, ink);
  stateMark(m, x + 6, 170, ink, dim, now);

  // ├ followers
  if (s.followerCount) {
    canvas.drawFastVLine(ox + 12, 190, 14, MID);
    canvas.drawFastHLine(ox + 12, 203, 6, MID);
    followers(s, ox + 22, DISP_W - 30, 214, 186, 2, true, MID, now);
  }
  // A waiting session frames the whole tube
  if (m == MOOD_WAITING && alert && inverse) {
    canvas.drawRect(ox, 0, DISP_W, DISP_H, AMBER);
    canvas.drawRect(ox + 1, 1, DISP_W - 2, DISP_H - 2, AMBER);
  }
}

// ── Hooks ───────────────────────────────────────────────────────

static bool idleShown = false;       // the boot log types out once each time the squad empties
static unsigned long idleSinceMs = 0;

static void page(uint8_t first, uint8_t count, int16_t ox, int16_t contentH, unsigned long now) {
  idleShown = false;
  background(ox, 0, contentH);
  if (sessionCount == 1 && count == 1) {
    solo(sessions[first], ox, now);
  } else {
    const RowLayout& L = rowLayout();
    for (uint8_t i = 0; i < count; i++) drawRow(sessions[first + i], L, ox, STATUS_H + i * L.h, now);
  }
  statusLine(ox, now);
  if (contentH > BAR_Y) promptLine(ox, BAR_Y, now);
}

// Terminal scroll: one row per frame, the next page's rows coming in from the bottom
static void transition(uint8_t from, uint8_t to, uint16_t progress, int16_t contentH, unsigned long now) {
  idleShown = false;
  background(0, 0, contentH);
  uint8_t shift = (progress * ROWS + TRANSITION_ONE / 2) / TRANSITION_ONE;
  for (uint8_t slot = 0; slot < ROWS; slot++) {
    uint8_t v = slot + shift;
    uint8_t pg = v < ROWS ? from : to;
    uint8_t i = pg * T->perPage + v % ROWS;
    if (v % ROWS < T->perPage && i < sessionCount) drawRow(sessions[i], COMPACT, 0, STATUS_H + slot * ROW_H, now);
  }
  statusLine(0, now);
}

static int16_t pagerHeight(uint8_t pages) { return pages > 1 ? BAR_H : 0; }

// -- 1/2 --, and a blinking page number for each other page that holds an alert
static void pager(uint8_t pages, uint8_t active, int16_t y, int16_t h, unsigned long now) {
  background(0, y, h);
  canvas.drawFastHLine(0, y, DISP_W, LO);
  char buf[16];
  snprintf(buf, sizeof(buf), "-- %u/%u --", active + 1, pages);
  int16_t w = labelFont.getUTF8Width(buf);
  text(buf, (DISP_W - w) / 2, y + 15, MID);
  text("$", 7, y + 15, MID);
  int16_t x = DISP_W - 8;
  for (int8_t p = pages - 1; p >= 0; p--) {
    if (p == active) continue;
    uint16_t col = 0;
    for (uint8_t i = p * T->perPage; i < sessionCount && i < (p + 1) * T->perPage; i++) {
      if (isAlerting(sessions[i])) col = sessions[i].mood == MOOD_WAITING ? AMBER : col ? col : RED;
    }
    if (!col) continue;
    char n[4];
    snprintf(n, sizeof(n), "%u", p + 1);
    x -= 12;
    if (blink(now, 600)) {
      canvas.fillRect(x, y + 5, 11, 11, col);
      small(n, x + 3, y + 7, BLACK);
    } else {
      canvas.drawRect(x, y + 5, 11, 11, col);
      small(n, x + 3, y + 7, col);
    }
    x -= 2;
  }
  corners(false, true);
}

// Nobody connected: a boot log types itself out, then waits with a cursor; Clawd dozes in the corner
static void idleScreen(unsigned long now) {
  if (!idleShown) {
    idleShown = true;
    idleSinceMs = now;
  }
  unsigned long t = now - idleSinceMs;
  background(0, 0, DISP_H);

  const int16_t x = 10;
  // Characters appear at 1 px per 2 ms of their line's turn; lines start 300 ms apart
  auto typed = [&](uint8_t line) -> int16_t {
    unsigned long start = line * 300UL;
    return t < start ? -1 : (int16_t)min((t - start) / 2, 220UL);
  };
  int16_t r = typed(0);
  if (r >= 0) {
    char head[12] = "CLAWD-MOOD";
    head[min((int)(r / 12), 10)] = 0;
    small(head, x, 14, HI, 2);
    canvas.drawFastHLine(x, 34, min((int)r, DISP_W - 2 * x), LO);
  }
  static const char* const LOG[3][2] = {{"> 主题  ", "荧光终端"}, {"> 串口  ", "115200"}, {"> 会话  ", "0"}};
  int16_t cursorX = x, cursorY = 46;
  for (uint8_t i = 0; i < 3; i++) {
    r = typed(i + 1);
    if (r < 0) break;
    int16_t base = 52 + i * 18;
    int16_t w = drawLabel(LOG[i][0], x, base, r, MID);
    if (r > w) w += drawLabel(LOG[i][1], x + w, base, r - w, HI);
    cursorX = x + w + 2;
    cursorY = base;
  }
  r = typed(4);
  if (r >= 0) {
    int16_t w = drawLabel("等待会话接入", x, 116, r, HI);
    if (t > 1700) {
      uint8_t dots = (now / 300) % 4;
      for (uint8_t d = 0; d < dots; d++) canvas.fillRect(x + w + 2 + d * 5, 114, 2, 2, HI);
    }
    cursorX = x + w + 2;
    cursorY = 116;
  }
  if (t > 1900) {
    int16_t w = text("$ ", x, 140, MID);
    cursorX = x + w;
    cursorY = 140;
  }
  cursorBlock(cursorX, cursorY, HI, now);

  // Clawd asleep on the bottom line, z's drifting up from it
  const int16_t ground = 214, u = 8;
  for (int16_t gx = 8; gx < DISP_W - 8; gx += 6) canvas.drawFastHLine(gx, ground + 2, 3, LO);
  drawCreature("CC", 0, MOOD_SLEEPING, 0, false, 124, ground, u, now);
  for (uint8_t i = 0; i < 3; i++) {
    uint16_t p = (now + i * 400) % 1200;
    small("z", 200 + p / 60, 170 - p / 30, p < 800 ? MID : LO, p < 400 ? 1 : 2);
  }
}

static void overlay(unsigned long) { corners(true, true); }

// The only confirmation of a cfg line: an inverse box, one line per changed setting
static void toast(const char* msg, unsigned long) {
  const char* parts[3];
  uint8_t lens[3], n = 0;
  for (const char* p = msg; *p && n < 3;) {
    const char* sep = strstr(p, "  ");
    size_t len = sep ? (size_t)(sep - p) : strlen(p);
    parts[n] = p;
    lens[n++] = (uint8_t)min(len, (size_t)40);
    if (!sep) break;
    p = sep + 2;
  }
  char line[3][41];
  int16_t widest = 0;
  for (uint8_t i = 0; i < n; i++) {
    memcpy(line[i], parts[i], lens[i]);
    line[i][lens[i]] = 0;
    widest = max(widest, (int16_t)labelFont.getUTF8Width(line[i]));
  }
  // A small terminal window: dark title strip, then the settings in black on bright green
  const int16_t pad = 14, lineH = 18, head = 16;
  int16_t w = min(max(widest + 2 * pad, 128), DISP_W - 16), h = head + n * lineH + 8;
  int16_t x = (DISP_W - w) / 2, y = (DISP_H - h) / 2;
  canvas.fillRect(x + 3, y + 3, w, h, BLACK);   // shadow, so the box lifts off whatever is under it
  canvas.fillRect(x, y, w, h, HI);
  canvas.fillRect(x + 2, y + 2, w - 4, head - 2, BLACK);
  text("设置", x + 7, y + 14, HI);
  for (uint8_t i = 0; i < n; i++) {
    int16_t lw = labelFont.getUTF8Width(line[i]);
    text(line[i], x + (w - lw) / 2, y + head + (i + 1) * lineH - 3, BLACK);
  }
}

static void enter(unsigned long) { idleShown = false; }

const Theme THEME_PHOSPHOR = {
  3, "phosphor", "荧光终端", "绿色荧光屏上的会话进程表", 0x33FF77, 2, 6, 0,
  {
    BG, SCAN, AMBER, RED,                  // bg gap waitFlash errorFrame
    BG, LO, HI, MID, MID,                  // plate tileBorder text tag more
    BG, HI, LO,                            // pagerBg pagerOn pagerOff
    MID, BG,                               // smoke sleepTarget
    HI, BLACK,                             // toastBg toastText
    {MID, HI, HI, AMBER, PALE, RED, LO},
    {PALE, PALE, PALE, PALE, PALE, PALE},  // unknown agents: Clawd in a paler green, told apart by tag
    {PALE, HI, MID, PALE, HI, MID},
  },
  SKINS, KNOT_LOGO,
  {I_GEAR_A, I_GEAR_B, I_KEYS_A, I_KEYS_B, I_ASK, I_CHECK, I_CROSS, I_ZZZ},
  {nullptr, nullptr, idleScreen, overlay, page, transition, pagerHeight, pager, toast, enter},
  {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}},
};
