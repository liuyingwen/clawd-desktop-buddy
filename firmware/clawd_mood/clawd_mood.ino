/*
 * clawd-mood — ESP32-C3 Super Mini + ST7789 1.54" 240x240
 *
 * Desktop squad: every CLI session is a pixel creature with a name and a
 * state. Up to 4 per page (filling the screen), more pages rotate on their own.
 */
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <U8g2_for_Adafruit_GFX.h>

#define TFT_CS  4
#define TFT_DC  1
#define TFT_RST 2
#define TFT_BLK 3

#define DISP_W 240
#define DISP_H 240

#define DONE_REVERT_MS    3000UL           // Done → Idle after 3s
#define SLEEP_IDLE_MS     (5UL*60*1000)    // 5 minutes without events
#define SESSION_EXPIRE_MS (30UL*60*1000)   // forget sessions that never sent SessionEnd

#define MAX_SESSIONS   12
#define PER_PAGE       4
#define PAGE_ROTATE_MS 5000UL
#define URGENT_HOLD_MS 10000UL
#define SLIDE_FRAMES   6
#define SLIDE_FRAME_MS 50UL
#define FRAME_MS       100UL
#define PAGER_H        4

#define LINE_MAX 320

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 canvas(DISP_W, DISP_H);   // full-screen buffer: no flicker between cells
U8G2_FOR_ADAFRUIT_GFX labelFont;       // Chinese-capable text for session labels

uint16_t C_CREAM, C_DIVIDER, C_INK;
uint16_t C_IDLE, C_THINKING, C_WORKING, C_WAITING, C_DONE, C_ERROR, C_SLEEPING;
uint16_t bodyColors[6];

enum Mood {
  MOOD_IDLE, MOOD_THINKING, MOOD_WORKING, MOOD_WAITING,
  MOOD_DONE, MOOD_ERROR, MOOD_SLEEPING, MOOD_UNKNOWN
};

struct Session {
  char id[9];
  char name[65];    // working directory, UTF-8
  char title[65];   // session title, kept once known
  char src[3];
  Mood mood;
  uint8_t color;
  unsigned long lastEventMs;
  unsigned long moodSinceMs;
};

Session sessions[MAX_SESSIONS];
uint8_t sessionCount = 0;
uint8_t nextColor = 0;

uint8_t currentPage = 0;
int8_t slideTo = -1;                // target page while sliding, -1 = not sliding
unsigned long slideStartMs = 0;
unsigned long lastPageMs = 0;
char urgentId[9] = "";              // session the screen is holding on
unsigned long urgentUntilMs = 0;

unsigned long lastFrameMs = 0;
char lineBuf[LINE_MAX];
size_t lineLen = 0;

void initColors() {
  C_CREAM    = tft.color565(255, 241, 214);
  C_DIVIDER  = tft.color565(40, 34, 30);
  C_INK      = ST77XX_BLACK;
  C_IDLE     = tft.color565(150, 150, 150);
  C_THINKING = tft.color565(40, 110, 240);
  C_WORKING  = tft.color565(30, 170, 70);
  C_WAITING  = tft.color565(255, 205, 0);
  C_DONE     = ST77XX_WHITE;
  C_ERROR    = tft.color565(225, 30, 30);
  C_SLEEPING = tft.color565(70, 70, 76);
  bodyColors[0] = tft.color565(240, 120, 30);   // orange
  bodyColors[1] = tft.color565(30, 170, 170);   // teal
  bodyColors[2] = tft.color565(235, 90, 150);   // pink
  bodyColors[3] = tft.color565(130, 90, 210);   // purple
  bodyColors[4] = tft.color565(150, 110, 60);   // brown
  bodyColors[5] = tft.color565(90, 140, 40);    // moss
}

uint16_t stateColor(Mood m) {
  switch (m) {
    case MOOD_THINKING: return C_THINKING;
    case MOOD_WORKING:  return C_WORKING;
    case MOOD_WAITING:  return C_WAITING;
    case MOOD_DONE:     return C_DONE;
    case MOOD_ERROR:    return C_ERROR;
    case MOOD_SLEEPING: return C_SLEEPING;
    default:            return C_IDLE;
  }
}

Mood parseMood(const char* s) {
  if (!s) return MOOD_UNKNOWN;
  if (!strcmp(s, "idle"))     return MOOD_IDLE;
  if (!strcmp(s, "thinking")) return MOOD_THINKING;
  if (!strcmp(s, "working"))  return MOOD_WORKING;
  if (!strcmp(s, "waiting"))  return MOOD_WAITING;
  if (!strcmp(s, "done"))     return MOOD_DONE;
  if (!strcmp(s, "error"))    return MOOD_ERROR;
  if (!strcmp(s, "sleeping")) return MOOD_SLEEPING;
  return MOOD_UNKNOWN;
}

inline bool isUrgent(Mood m) { return m == MOOD_WAITING || m == MOOD_ERROR; }

// ── Session table ───────────────────────────────────────────────
// Positions are stable: new sessions append, states never reorder.

int8_t findSession(const char* id) {
  for (uint8_t i = 0; i < sessionCount; i++) {
    if (!strncmp(sessions[i].id, id, sizeof(sessions[i].id) - 1)) return i;
  }
  return -1;
}

void removeSession(uint8_t i) {
  for (uint8_t j = i; j + 1 < sessionCount; j++) sessions[j] = sessions[j + 1];
  sessionCount--;
}

int8_t addSession(const char* id, const char* name, const char* src) {
  if (sessionCount == MAX_SESSIONS) {
    // Table full: drop the quietest non-urgent session
    int8_t victim = -1;
    for (uint8_t i = 0; i < sessionCount; i++) {
      if (isUrgent(sessions[i].mood)) continue;
      if (victim < 0 || sessions[i].lastEventMs < sessions[victim].lastEventMs) victim = i;
    }
    if (victim < 0) return -1;
    removeSession(victim);
  }
  Session& s = sessions[sessionCount];
  strlcpy(s.id, id, sizeof(s.id));
  strlcpy(s.name, name, sizeof(s.name));
  strlcpy(s.src, src, sizeof(s.src));
  s.title[0] = 0;
  s.mood = MOOD_IDLE;
  s.color = nextColor++ % 6;
  s.moodSinceMs = millis();
  return sessionCount++;
}

void setMood(Session& s, Mood m) {
  if (m == s.mood) return;
  s.mood = m;
  s.moodSinceMs = millis();
}

void tickSessions() {
  unsigned long now = millis();
  for (int8_t i = sessionCount - 1; i >= 0; i--) {
    Session& s = sessions[i];
    unsigned long quiet = now - s.lastEventMs;
    if (quiet > SESSION_EXPIRE_MS) {
      removeSession(i);
      continue;
    }
    if (s.mood == MOOD_DONE && now - s.moodSinceMs > DONE_REVERT_MS) setMood(s, MOOD_IDLE);
    // A pending approval stays visible until it is answered or the session expires
    if (s.mood != MOOD_WAITING && s.mood != MOOD_SLEEPING && quiet > SLEEP_IDLE_MS) {
      setMood(s, MOOD_SLEEPING);
    }
  }
}

void handleLine(const char* line) {
  if (!line[0]) return;
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    Serial.print("[warn] bad json: "); Serial.println(line);
    return;
  }
  const char* id = doc["session"] | "default";
  if (doc["end"] | false) {
    int8_t i = findSession(id);
    if (i >= 0) removeSession(i);
    return;
  }
  const char* st = doc["state"] | "";
  Mood next = parseMood(st);
  if (next == MOOD_UNKNOWN) {
    Serial.print("[warn] unknown state: "); Serial.println(st);
    return;
  }
  int8_t i = findSession(id);
  if (i < 0) i = addSession(id, doc["name"] | "clawd", doc["src"] | "");
  if (i < 0) return;
  Session& s = sessions[i];
  const char* name = doc["name"];
  const char* title = doc["title"];
  if (name) strlcpy(s.name, name, sizeof(s.name));
  if (title) strlcpy(s.title, title, sizeof(s.title));
  Mood prev = s.mood;
  setMood(s, next);
  s.lastEventMs = millis();
  if (isUrgent(next) && !isUrgent(prev)) {
    strlcpy(urgentId, s.id, sizeof(urgentId));
    urgentUntilMs = millis() + URGENT_HOLD_MS;
  }
}

void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      lineBuf[lineLen] = 0;
      handleLine(lineBuf);
      lineLen = 0;
    } else if (c != '\r' && lineLen < LINE_MAX - 1) {
      lineBuf[lineLen++] = c;
    }
  }
}

// ── Paging ──────────────────────────────────────────────────────

inline uint8_t pageCount() { return sessionCount ? (sessionCount + PER_PAGE - 1) / PER_PAGE : 1; }

void startSlide(uint8_t to) {
  slideTo = to;
  slideStartMs = millis();
  lastPageMs = slideStartMs;
}

void tickPaging() {
  unsigned long now = millis();
  uint8_t pages = pageCount();
  if (currentPage >= pages) currentPage = pages - 1;
  if (slideTo >= pages) slideTo = -1;
  if (pages == 1) {
    slideTo = -1;
    return;
  }
  if (slideTo >= 0) {
    if (now - slideStartMs < SLIDE_FRAMES * SLIDE_FRAME_MS) return;
    currentPage = slideTo;
    slideTo = -1;
    lastPageMs = now;
  }
  if (urgentId[0]) {
    int8_t i = findSession(urgentId);
    if (i >= 0 && isUrgent(sessions[i].mood) && (long)(urgentUntilMs - now) > 0) {
      uint8_t target = i / PER_PAGE;
      if (target != currentPage) startSlide(target);
      lastPageMs = now;
      return;
    }
    urgentId[0] = 0;
  }
  if (now - lastPageMs > PAGE_ROTATE_MS) startSlide((currentPage + 1) % pages);
}

// ── Drawing ─────────────────────────────────────────────────────

// 7x7 state icons, one byte per row, bit 6 is the leftmost pixel
const uint8_t ICON_GEAR_A[7]  = {0x08,0x5D,0x3E,0x77,0x3E,0x5D,0x08};
const uint8_t ICON_GEAR_B[7]  = {0x22,0x1C,0x3E,0x36,0x3E,0x1C,0x22};
const uint8_t ICON_KEYS_A[7]  = {0x00,0x7F,0x55,0x7F,0x6B,0x7F,0x00};
const uint8_t ICON_KEYS_B[7]  = {0x00,0x7F,0x6B,0x7F,0x55,0x7F,0x00};
const uint8_t ICON_ASK[7]     = {0x3E,0x63,0x03,0x0E,0x08,0x00,0x08};
const uint8_t ICON_CHECK[7]   = {0x00,0x01,0x03,0x46,0x6C,0x38,0x10};
const uint8_t ICON_CROSS[7]   = {0x63,0x77,0x3E,0x1C,0x3E,0x77,0x63};
const uint8_t ICON_ZZZ[7]     = {0x7F,0x03,0x06,0x0C,0x18,0x30,0x7F};

void drawIcon(int16_t x, int16_t y, const uint8_t* rows, uint8_t u, uint16_t col) {
  // Black plate keeps every state colour readable on any cell background
  canvas.fillRect(x - u, y - u, 9 * u, 9 * u, C_INK);
  for (uint8_t r = 0; r < 7; r++) {
    for (uint8_t c = 0; c < 7; c++) {
      if (rows[r] & (0x40 >> c)) canvas.fillRect(x + c * u, y + r * u, u, u, col);
    }
  }
}

uint16_t dimColor(uint16_t c) { return (c >> 1) & 0x7BEF; }

// Creature grid is 12 wide x 9 tall; (x, feetY) is its bottom-left corner.
void drawCreature(const Session& s, int16_t x, int16_t feetY, uint8_t u, unsigned long now) {
  uint16_t body = bodyColors[s.color];
  unsigned long t = now - s.moodSinceMs;

  if (s.mood == MOOD_SLEEPING) {     // lying flat, eyes shut
    body = dimColor(body);
    canvas.fillRect(x, feetY - 4 * u, 12 * u, 4 * u, body);
    canvas.fillRect(x + 2 * u, feetY - 2 * u - u / 2, 3 * u, u / 2 + 1, C_INK);
    canvas.fillRect(x + 7 * u, feetY - 2 * u - u / 2, 3 * u, u / 2 + 1, C_INK);
    return;
  }

  int16_t dx = 0, dy = 0;
  switch (s.mood) {
    case MOOD_IDLE: {
      const int8_t wander[] = {0, -1, 0, 1};
      dx = wander[(now / 800) % 4] * u / 2;
      break;
    }
    case MOOD_WORKING: dy = -((now / 150) % 2) * (u / 3 + 1); break;
    case MOOD_WAITING: dy = -((now / 250) % 2) * u; break;
    case MOOD_DONE:    dy = t < 1500 ? -((now / 150) % 2) * u : 0; break;
    case MOOD_ERROR:   dx = (((now / 80) * 17) % 3 - 1) * (u / 3 + 1); break;
    default: break;
  }
  int16_t bx = x + dx, by = feetY - 9 * u + dy;

  // Jumps lift the legs off the ground; otherwise they stretch down to it
  bool up = s.mood == MOOD_WAITING || s.mood == MOOD_DONE;
  int16_t legY = by + 7 * u;
  int16_t legH = up ? 2 * u : feetY - legY;
  canvas.fillRect(bx + 2 * u, legY, 2 * u, legH, body);
  canvas.fillRect(bx + 8 * u, legY, 2 * u, legH, body);
  canvas.fillRect(bx + u, by, 10 * u, 7 * u, body);

  // Arms: typing alternates, waiting and done throw them up
  int16_t lArm = 3, rArm = 3;
  if (s.mood == MOOD_WORKING) { bool f = (now / 150) % 2; lArm = f ? 2 : 4; rArm = f ? 4 : 2; }
  if (up) { lArm = 1; rArm = 1; }
  canvas.fillRect(bx, by + lArm * u, u, 2 * u, body);
  canvas.fillRect(bx + 11 * u, by + rArm * u, u, 2 * u, body);

  // Eyes
  int16_t ly = by + 2 * u, lx = bx + 3 * u, rx = bx + 7 * u;
  switch (s.mood) {
    case MOOD_DONE:      // happy ^ ^
      canvas.fillRect(lx,     ly + u, u, u, C_INK);
      canvas.fillRect(lx + u, ly,     u, u, C_INK);
      canvas.fillRect(rx,     ly + u, u, u, C_INK);
      canvas.fillRect(rx + u, ly,     u, u, C_INK);
      break;
    case MOOD_ERROR:     // X X
      for (uint8_t i = 0; i < 2; i++) {
        canvas.fillRect(lx + i * u, ly + i * u,       u, u, C_INK);
        canvas.fillRect(lx + i * u, ly + (1 - i) * u, u, u, C_INK);
        canvas.fillRect(rx + i * u, ly + i * u,       u, u, C_INK);
        canvas.fillRect(rx + i * u, ly + (1 - i) * u, u, u, C_INK);
      }
      break;
    case MOOD_THINKING: {  // look around
      const int8_t look[] = {0, -1, 1, 0};
      int16_t e = look[(now / 600) % 4] * u / 2;
      canvas.fillRect(lx + e, ly, 2 * u, 2 * u, C_INK);
      canvas.fillRect(rx + e, ly, 2 * u, 2 * u, C_INK);
      break;
    }
    default: {
      // Blink every ~6s for idle, otherwise eyes stay open
      bool blink = s.mood == MOOD_IDLE && (now % 6000) < 200;
      int16_t h = blink ? u / 2 + 1 : 2 * u;
      int16_t off = blink ? u : 0;
      canvas.fillRect(lx, ly + off, 2 * u, h, C_INK);
      canvas.fillRect(rx, ly + off, 2 * u, h, C_INK);
    }
  }

  if (s.mood == MOOD_DONE && t < 1500) {   // confetti
    for (uint8_t i = 0; i < 6; i++) {
      int16_t cx = x + (i * 37 + now / 40) % (12 * u);
      int16_t cy = by + ((i * 53 + now / 15) % (4 * u));
      canvas.fillRect(cx, cy, u / 2 + 1, u / 2 + 1, bodyColors[i % 6]);
    }
  }
  if (s.mood == MOOD_ERROR) {              // smoke
    int16_t sy = by - ((now / 120) % 3) * u / 2;
    canvas.fillRect(bx + 10 * u, sy, u, u, C_SLEEPING);
    canvas.fillRect(bx + 11 * u, sy - u, u / 2 + 1, u / 2 + 1, C_SLEEPING);
  }
}

const uint8_t* iconFor(Mood m, unsigned long now) {
  switch (m) {
    case MOOD_THINKING: return (now / 300) % 2 ? ICON_GEAR_A : ICON_GEAR_B;
    case MOOD_WORKING:  return (now / 200) % 2 ? ICON_KEYS_A : ICON_KEYS_B;
    case MOOD_WAITING:  return ICON_ASK;
    case MOOD_DONE:     return ICON_CHECK;
    case MOOD_ERROR:    return ICON_CROSS;
    case MOOD_SLEEPING: return ICON_ZZZ;
    default:            return nullptr;
  }
}

void drawCell(const Session& s, int16_t x, int16_t y, int16_t w, int16_t h, unsigned long now) {
  uint16_t bg = (s.mood == MOOD_WAITING && (now / 500) % 2) ? C_WAITING : C_CREAM;
  canvas.fillRect(x, y, w, h, bg);

  const int16_t labelH = 15, tileH = 6;
  int16_t tileY = y + h - labelH - tileH;

  // Largest creature that fits above the tile together with its icon
  uint8_t u = 2, iu = 2;
  for (uint8_t t = 12; t >= 2; t--) {
    uint8_t ti = t / 2 < 2 ? 2 : t / 2;
    if (12 * t <= w - 8 && 10 * t + 9 * ti + 2 <= tileY - y) { u = t; iu = ti; break; }
  }

  canvas.fillRect(x + 4, tileY, w - 8, tileH, stateColor(s.mood));
  canvas.drawRect(x + 4, tileY, w - 8, tileH, C_INK);

  drawCreature(s, x + (w - 12 * u) / 2, tileY, u, now);

  const uint8_t* icon = iconFor(s.mood, now);
  if (icon) {
    drawIcon(x + (w - 7 * iu) / 2, y + iu + 1, icon, iu, stateColor(s.mood));
  }

  // Session title (directory until one exists), cut on a UTF-8 boundary to fit
  char label[65];
  strlcpy(label, s.title[0] ? s.title : s.name, sizeof(label));
  size_t len = strlen(label);
  while (len && labelFont.getUTF8Width(label) > w - 4) {
    do { len--; } while (len && (label[len] & 0xC0) == 0x80);
    label[len] = 0;
  }
  labelFont.setForegroundColor(C_INK);
  labelFont.drawUTF8(x + (w - labelFont.getUTF8Width(label)) / 2, tileY + tileH + 12, label);

  if (s.src[0]) {
    canvas.setTextSize(1);
    canvas.setTextColor(C_INK);
    canvas.setCursor(x + 3, y + 3);
    canvas.print(s.src);
  }
  if (s.mood == MOOD_ERROR) {
    canvas.drawRect(x, y, w, h, C_ERROR);
    canvas.drawRect(x + 1, y + 1, w - 2, h - 2, C_ERROR);
  }
}

void drawPage(uint8_t page, int16_t ox, int16_t contentH, unsigned long now) {
  uint8_t first = page * PER_PAGE;
  uint8_t n = sessionCount > first ? min(sessionCount - first, PER_PAGE) : 0;
  const int16_t half = (DISP_W - 2) / 2, right = half + 2;
  const int16_t halfH = (contentH - 2) / 2, lower = contentH - halfH;

  canvas.fillRect(ox, 0, DISP_W, contentH, C_DIVIDER);
  const Session* s = sessions + first;
  switch (n) {
    case 1:
      drawCell(s[0], ox, 0, DISP_W, contentH, now);
      break;
    case 2:
      drawCell(s[0], ox, 0, half, contentH, now);
      drawCell(s[1], ox + right, 0, half, contentH, now);
      break;
    case 3:
      drawCell(s[0], ox, 0, half, contentH, now);
      drawCell(s[1], ox + right, 0, half, halfH, now);
      drawCell(s[2], ox + right, lower, half, halfH, now);
      break;
    case 4:
      drawCell(s[0], ox, 0, half, halfH, now);
      drawCell(s[1], ox + right, 0, half, halfH, now);
      drawCell(s[2], ox, lower, half, halfH, now);
      drawCell(s[3], ox + right, lower, half, halfH, now);
      break;
  }
}

void drawPager(uint8_t pages, uint8_t active, unsigned long now) {
  canvas.fillRect(0, DISP_H - PAGER_H, DISP_W, PAGER_H, C_DIVIDER);
  const int16_t dotW = 16, gap = 6;
  int16_t x = (DISP_W - pages * dotW - (pages - 1) * gap) / 2;
  for (uint8_t p = 0; p < pages; p++, x += dotW + gap) {
    uint16_t col = p == active ? C_DONE : C_SLEEPING;
    // Pages holding an urgent session blink in that state's colour
    for (uint8_t i = p * PER_PAGE; i < sessionCount && i < (p + 1) * PER_PAGE; i++) {
      if (isUrgent(sessions[i].mood) && (now / 250) % 2) col = stateColor(sessions[i].mood);
    }
    canvas.fillRect(x, DISP_H - PAGER_H, dotW, PAGER_H, col);
  }
}

void render() {
  unsigned long now = millis();
  uint8_t pages = pageCount();
  int16_t contentH = pages > 1 ? DISP_H - PAGER_H : DISP_H;

  if (sessionCount == 0) {           // nobody connected: one sleeping creature
    Session none = {};
    strlcpy(none.name, "no session", sizeof(none.name));
    none.mood = MOOD_SLEEPING;
    drawCell(none, 0, 0, DISP_W, DISP_H, now);
  } else if (slideTo >= 0) {
    uint8_t frame = min((now - slideStartMs) / SLIDE_FRAME_MS + 1, (unsigned long)SLIDE_FRAMES);
    int16_t shift = DISP_W * frame / SLIDE_FRAMES;
    drawPage(currentPage, -shift, contentH, now);
    drawPage(slideTo, DISP_W - shift, contentH, now);
  } else {
    drawPage(currentPage, 0, contentH, now);
  }
  if (pages > 1) drawPager(pages, slideTo >= 0 ? slideTo : currentPage, now);

  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), DISP_W, DISP_H);
}

void setup() {
  Serial.begin(115200);
  pinMode(TFT_BLK, OUTPUT); digitalWrite(TFT_BLK, HIGH);
  SPI.begin(8, -1, 10, TFT_CS);   // SCK=8, MOSI=10 (ESP32-C3 pin remap)
  tft.setSPISpeed(40000000);
  tft.init(DISP_W, DISP_H);
  tft.setRotation(1);
  initColors();
  labelFont.begin(canvas);
  labelFont.setFont(u8g2_font_wqy12_t_gb2312);   // resets font mode, so set it first
  labelFont.setFontMode(1);
  if (!canvas.getBuffer()) Serial.println("[warn] no RAM for frame buffer");
}

void loop() {
  pollSerial();
  tickSessions();
  tickPaging();
  unsigned long now = millis();
  unsigned long frameMs = slideTo >= 0 ? SLIDE_FRAME_MS : FRAME_MS;
  if (canvas.getBuffer() && now - lastFrameMs >= frameMs) {
    lastFrameMs = now;
    render();
  }
  delay(5);
}
