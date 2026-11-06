/*
 * clawd-mood — ESP32-C3 Super Mini + ST7789 1.54" 240x240
 *
 * Desktop squad: every CLI session is a pixel creature with a name and a
 * state. A theme sets how many share a page (4 by default, filling the
 * screen); more pages rotate on their own.
 */
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <Preferences.h>
#include "theme.h"
#include "ble_link.h"

#define TFT_CS  4
#define TFT_DC  1
#define TFT_RST 2
#define TFT_BLK 3

#define DONE_REVERT_MS    3000UL           // Done → Idle after 3s
#define SLEEP_IDLE_MS     (5UL*60*1000)    // 5 minutes without events
#define SLEEP_WORKING_MS  (15UL*60*1000)   // a long tool call sends nothing while it runs
#define SESSION_EXPIRE_MS (30UL*60*1000)   // forget sessions that never sent SessionEnd

#define FOLLOWER_LEAVE_MS  1500UL         // celebrates this long after SubagentStop
#define FOLLOWER_EXPIRE_MS (15UL*60*1000) // same patience as a long tool call
#define FOLLOWER_PAUSE_MS  (2UL*60*1000)  // no tool running: finished, or it comes back on its next tool
#define FOLLOWER_DROP_MS   400UL          // falls into line when it appears
#define URGENT_HOLD_MS 10000UL

#define LINE_MAX 320
#define SERIAL_RX_BYTES 4096  // input is only read between frames; the default 256 loses lines

#define DEFAULT_ROTATION   1
#define DEFAULT_BRIGHTNESS 100
#define MIN_BRIGHTNESS     10
#define MAX_BRIGHTNESS     100
#define BLK_PWM_HZ         5000
#define BLK_PWM_BITS       8
#define SAVE_DELAY_MS      1500UL   // dragging a slider costs one flash write, not one per step
#define TOAST_MS           3000UL

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 canvas(DISP_W, DISP_H);   // full-screen buffer: no flicker between cells
U8G2_FOR_ADAFRUIT_GFX labelFont;       // Chinese-capable text for session labels

#define THEME_ENTRY(t) &t,
const Theme* const THEMES[] = {THEME_TABLE(THEME_ENTRY)};
const Theme* T = THEMES[0];

// Follower and Session live in theme.h: themes read them.
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

// Serial and Bluetooth each get their own buffer: a line half-received on one must not
// swallow bytes from the other.
struct LineReader {
  char buf[LINE_MAX];
  size_t len = 0;
  void feed(char c);
};
LineReader serialLines, bleLines;

uint16_t stateColor(Mood m) { return T->pal.state[m < MOOD_UNKNOWN ? m : MOOD_IDLE]; }

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

bool isAlerting(const Session& s) { return isUrgent(s.mood) && !s.quiet; }

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
  s.mood = s.own = MOOD_IDLE;
  s.beforeCompact = MOOD_UNKNOWN;
  s.followerCount = 0;
  s.color = nextColor++ % 6;
  s.moodSinceMs = s.ownSinceMs = millis();
  s.quiet = false;
  return sessionCount++;
}

void setMood(Session& s, Mood m) {
  if (m == s.mood) return;
  s.mood = m;
  s.moodSinceMs = millis();
}

// A subagent waiting for approval must be as visible as the main agent waiting
void syncMood(Session& s) {
  Mood shown = s.own;
  for (uint8_t i = 0; i < s.followerCount; i++) {
    if (!s.followers[i].leaving && s.followers[i].mood == MOOD_WAITING) shown = MOOD_WAITING;
  }
  setMood(s, shown);
}

void setOwn(Session& s, Mood m) {
  if (m != s.own) {
    s.own = m;
    s.ownSinceMs = millis();
  }
  syncMood(s);
}

void removeFollower(Session& s, uint8_t i) {
  for (uint8_t j = i; j + 1 < s.followerCount; j++) s.followers[j] = s.followers[j + 1];
  s.followerCount--;
}

int8_t findFollower(const Session& s, const char* id) {
  for (uint8_t i = 0; i < s.followerCount; i++) {
    if (!strncmp(s.followers[i].id, id, sizeof(s.followers[i].id) - 1)) return i;
  }
  return -1;
}

void handleFollower(Session& s, const char* id, Mood next, bool gone, const char* event) {
  unsigned long now = millis();
  int8_t i = findFollower(s, id);
  if (gone) {
    if (i >= 0 && !s.followers[i].leaving) {
      s.followers[i].leaving = true;
      s.followers[i].mood = MOOD_DONE;
      s.followers[i].moodSinceMs = now;
    }
    return;
  }
  if (i >= 0 && s.followers[i].leaving) return;  // hooks are async; ignore stragglers
  if (i < 0) {
    if (s.followerCount == MAX_FOLLOWERS) {
      uint8_t oldest = 0;
      for (uint8_t j = 1; j < s.followerCount; j++) {
        if (s.followers[j].lastEventMs < s.followers[oldest].lastEventMs) oldest = j;
      }
      removeFollower(s, oldest);
    }
    i = s.followerCount++;
    Follower& n = s.followers[i];
    strlcpy(n.id, id, sizeof(n.id));
    n.mood = MOOD_UNKNOWN;
    n.leaving = false;
    n.bornMs = now;
  }
  Follower& f = s.followers[i];
  if (next != MOOD_THINKING && next != MOOD_WAITING) next = MOOD_WORKING;
  if (f.mood != next) {
    f.mood = next;
    f.moodSinceMs = now;
  }
  f.lastEventMs = now;
  f.betweenTools = event && (!strcmp(event, "PostToolUse") || !strcmp(event, "PostToolUseFailure"));
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
    for (int8_t j = s.followerCount - 1; j >= 0; j--) {
      const Follower& f = s.followers[j];
      unsigned long patience = f.betweenTools ? FOLLOWER_PAUSE_MS : FOLLOWER_EXPIRE_MS;
      if (f.leaving ? now - f.moodSinceMs > FOLLOWER_LEAVE_MS : now - f.lastEventMs > patience) {
        removeFollower(s, j);
      }
    }
    if (s.own == MOOD_DONE && now - s.ownSinceMs > DONE_REVERT_MS) s.own = MOOD_IDLE, s.ownSinceMs = now;
    // A pending approval stays visible until it is answered or the session expires
    unsigned long sleepAfter = s.own == MOOD_WORKING ? SLEEP_WORKING_MS : SLEEP_IDLE_MS;
    if (s.own != MOOD_WAITING && s.own != MOOD_SLEEPING && quiet > sleepAfter) {
      s.own = MOOD_SLEEPING;
      s.ownSinceMs = now;
    }
    syncMood(s);
  }
}

// ── Settings ────────────────────────────────────────────────────
// {"cfg":{"theme":N,"rotation":R,"brightness":B}}, every key optional.

Preferences prefs;
uint8_t rotation = DEFAULT_ROTATION;
uint8_t brightness = DEFAULT_BRIGHTNESS;
uint8_t savedTheme, savedRotation, savedBrightness;   // what NVS holds
// Keys a cfg changed. Only these are saved, so a value this firmware cannot use stays in NVS.
enum : uint8_t { DIRTY_THEME = 1, DIRTY_ROTATION = 2, DIRTY_BRIGHTNESS = 4 };
uint8_t settingsDirty = 0;
unsigned long settingsChangedMs = 0;
char toast[64] = "";
unsigned long toastUntilMs = 0;

const Theme* findTheme(int id) {
  for (const Theme* t : THEMES) {
    if (t->id == id) return t;
  }
  return nullptr;
}

// Every change of T goes through here. A different page size moves to the page holding the first
// session the screen showed and drops any page change, so no page index outlives its theme.
void useTheme(const Theme* t, unsigned long now) {
  if (t->perPage != T->perPage) {
    uint8_t first = (slideTo >= 0 ? slideTo : currentPage) * T->perPage;
    currentPage = first / t->perPage;
    slideTo = -1;
    lastPageMs = now;
  }
  T = t;
  if (T->ops.enter) T->ops.enter(now);
}

void setBacklight(uint8_t percent) {
  uint32_t duty = percent * ((1UL << BLK_PWM_BITS) - 1) / 100;   // the core treats all ones as fully on
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(TFT_BLK, duty);
#else
  ledcWrite(0, duty);
#endif
}

void beginBacklight() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!ledcAttach(TFT_BLK, BLK_PWM_HZ, BLK_PWM_BITS)) {   // better fixed brightness than a dark screen
    pinMode(TFT_BLK, OUTPUT);
    digitalWrite(TFT_BLK, HIGH);
    return;
  }
#else
  ledcSetup(0, BLK_PWM_HZ, BLK_PWM_BITS);
  ledcAttachPin(TFT_BLK, 0);
#endif
  setBacklight(brightness);
}

void loadSettings() {
  prefs.begin("clawd-mood", false);
  savedTheme = prefs.getUChar("theme", THEMES[0]->id);
  savedRotation = prefs.getUChar("rotation", DEFAULT_ROTATION);
  savedBrightness = prefs.getUChar("brightness", DEFAULT_BRIGHTNESS);
  prefs.end();
  // An id this firmware lacks (downgrade) draws theme 1 but stays saved, for the next upgrade
  const Theme* t = findTheme(savedTheme);
  useTheme(t ? t : THEMES[0], millis());
  rotation = savedRotation <= 3 ? savedRotation : DEFAULT_ROTATION;
  brightness = constrain(savedBrightness, MIN_BRIGHTNESS, MAX_BRIGHTNESS);
}

void tickSettingsSave() {
  if (!settingsDirty || millis() - settingsChangedMs < SAVE_DELAY_MS) return;
  bool theme = (settingsDirty & DIRTY_THEME) && T->id != savedTheme;
  bool rot = (settingsDirty & DIRTY_ROTATION) && rotation != savedRotation;
  bool bright = (settingsDirty & DIRTY_BRIGHTNESS) && brightness != savedBrightness;
  settingsDirty = 0;
  if (!theme && !rot && !bright) return;
  prefs.begin("clawd-mood", false);
  if (theme) prefs.putUChar("theme", savedTheme = T->id);
  if (rot) prefs.putUChar("rotation", savedRotation = rotation);
  if (bright) prefs.putUChar("brightness", savedBrightness = brightness);
  prefs.end();
}

// The link is one-way, so the toast is the only confirmation the host's user gets
void handleCfg(JsonObjectConst cfg) {
  char msg[sizeof(toast)] = "";
  size_t n = 0;
  if (cfg["theme"].is<int>()) {
    const Theme* t = findTheme(cfg["theme"].as<int>());
    if (!t) t = THEMES[0];
    if (t != T) {
      useTheme(t, millis());
      settingsDirty |= DIRTY_THEME;
      n += snprintf(msg + n, sizeof(msg) - n, "主题 %s  ", T->name);
    }
  }
  if (cfg["rotation"].is<int>()) {
    int r = cfg["rotation"].as<int>();
    if (r >= 0 && r <= 3 && r != rotation) {
      rotation = r;
      tft.setRotation(rotation);
      settingsDirty |= DIRTY_ROTATION;
      n += snprintf(msg + n, sizeof(msg) - n, "方向 %d°  ", rotation * 90);
    }
  }
  if (cfg["brightness"].is<float>()) {
    float b = constrain(cfg["brightness"].as<float>(), (float)MIN_BRIGHTNESS, (float)MAX_BRIGHTNESS);
    if ((uint8_t)b != brightness) {
      brightness = b;
      setBacklight(brightness);
      settingsDirty |= DIRTY_BRIGHTNESS;
      n += snprintf(msg + n, sizeof(msg) - n, "亮度 %d%%  ", brightness);
    }
  }
  if (!n) return;
  msg[n - 2] = 0;   // the separator after the last item
  strlcpy(toast, msg, sizeof(toast));
  toastUntilMs = millis() + TOAST_MS;
  settingsChangedMs = millis();
}

void handleLine(const char* line) {
  if (!line[0]) return;
  StaticJsonDocument<512> doc;  // a line with a 16-character Chinese title needs ~290
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    Serial.print("[warn] bad json: "); Serial.println(line);
    return;
  }
  // A settings line is never a session: not created, not touched, not woken
  JsonVariantConst cfg = doc["cfg"];
  if (!cfg.isNull()) {
    if (cfg.is<JsonObjectConst>()) handleCfg(cfg);
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
  const char* agent = doc["agent"];
  bool gone = doc["gone"] | false;
  int8_t i = findSession(id);
  if (i < 0 && gone) return;  // don't bring back an expired session just to say goodbye
  if (i < 0) i = addSession(id, doc["name"] | "clawd", doc["src"] | "");
  if (i < 0) return;
  Session& s = sessions[i];
  const char* name = doc["name"];
  const char* title = doc["title"];
  // A subagent may work elsewhere (a worktree); the label belongs to the main agent
  if (name && !agent) strlcpy(s.name, name, sizeof(s.name));
  if (title && !agent) strlcpy(s.title, title, sizeof(s.title));
  Mood prev = s.mood;
  s.lastEventMs = millis();
  bool quiet = false;
  if (agent) {
    handleFollower(s, agent, next, gone, doc["event"]);
    if (s.own == MOOD_SLEEPING) setOwn(s, MOOD_IDLE);
    syncMood(s);
    next = s.mood;
  } else {
    // Compaction also runs between turns, where no Stop follows; only resume if a turn was on
    const char* event = doc["event"] | "";
    bool compacting = !strcmp(event, "PreCompact");
    if (compacting && s.beforeCompact == MOOD_UNKNOWN) s.beforeCompact = s.own;
    if (!strcmp(event, "PostCompact")) {
      Mood was = s.beforeCompact != MOOD_UNKNOWN ? s.beforeCompact : s.own;
      next = was == MOOD_WORKING || was == MOOD_THINKING ? MOOD_WORKING : MOOD_IDLE;
    }
    if (!compacting) s.beforeCompact = MOOD_UNKNOWN;
    setOwn(s, next);
    quiet = doc["quiet"] | false;
  }
  if (isUrgent(next) && !quiet && (!isUrgent(prev) || s.quiet)) {
    strlcpy(urgentId, s.id, sizeof(urgentId));
    urgentUntilMs = millis() + URGENT_HOLD_MS;
  }
  s.quiet = quiet;
}

void LineReader::feed(char c) {
  if (c == '\n') {
    buf[len] = 0;
    handleLine(buf);
    len = 0;
  } else if (c != '\r' && len < LINE_MAX - 1) {
    buf[len++] = c;
  }
}

void pollSerial() {
  while (Serial.available()) serialLines.feed(Serial.read());
}

void pollBle() {
  char chunk[128];
  size_t n;
  while ((n = bleRead(chunk, sizeof(chunk))) > 0) {
    for (size_t i = 0; i < n; i++) bleLines.feed(chunk[i]);
  }
}

// ── Paging ──────────────────────────────────────────────────────

uint8_t pageCount() { return sessionCount ? (sessionCount + T->perPage - 1) / T->perPage : 1; }

int8_t heldUrgent(unsigned long now) {
  if (!urgentId[0]) return -1;
  int8_t i = findSession(urgentId);
  return i >= 0 && isUrgent(sessions[i].mood) && (long)(urgentUntilMs - now) > 0 ? i : -1;
}

uint16_t slideProgress(unsigned long now) {
  if (slideTo < 0) return 0;
  unsigned long frame = min((now - slideStartMs) / SLIDE_FRAME_MS + 1, (unsigned long)SLIDE_FRAMES);
  return TRANSITION_ONE * frame / SLIDE_FRAMES;
}

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
    int8_t i = heldUrgent(now);
    if (i >= 0) {
      uint8_t target = i / T->perPage;
      if (target != currentPage) startSlide(target);
      lastPageMs = now;
      return;
    }
    urgentId[0] = 0;
  }
  if (now - lastPageMs > (T->pageMs ? T->pageMs : PAGE_ROTATE_MS)) startSlide((currentPage + 1) % pages);
}

// ── Drawing ─────────────────────────────────────────────────────

void drawIcon(int16_t x, int16_t y, const uint8_t* rows, uint8_t u, uint16_t col) {
  // The plate keeps every state colour readable on any cell background
  canvas.fillRect(x - u, y - u, 9 * u, 9 * u, T->pal.plate);
  for (uint8_t r = 0; r < 7; r++) {
    for (uint8_t c = 0; c < 7; c++) {
      if (rows[r] & (0x40 >> c)) canvas.fillRect(x + c * u, y + r * u, u, u, col);
    }
  }
}

uint16_t dimColor(uint16_t c) { return ((c >> 1) & 0x7BEF) + ((T->pal.sleepTarget >> 1) & 0x7BEF); }

void drawKnot(int16_t x, int16_t y, int16_t size, uint16_t col) {
  for (int16_t py = 0; py < size; py++) {
    uint32_t row = T->knot[py * 30 / size];
    for (int16_t px = 0; px < size; px++) {
      if (row & (1UL << (29 - px * 30 / size))) canvas.fillRect(x + px, y + py, 1, 1, col);
    }
  }
}

const Skin& skinFor(const char* src) {
  for (uint8_t i = 0; i < SKIN_COUNT; i++) {
    if (!strcmp(T->skins[i].src, src)) return T->skins[i];
  }
  return T->skins[0];
}

void drawTorso(const Skin& k, uint16_t body, uint16_t accent, uint16_t face,
               int16_t x, int16_t y, uint8_t u, uint8_t step) {
  for (uint8_t r = 0; r * step < 7; r++) {
    for (uint8_t c = 0; c < 12; c++) {
      char p = k.rows[r * step][c];
      if (p != 'B' && p != 'A' && p != 'F') continue;
      canvas.fillRect(x + c * u, y + r * u, u, u, p == 'B' ? body : p == 'A' ? accent : face);
    }
  }
}

void drawEye(const Skin& k, int16_t ex, int16_t ey, uint8_t u, Mood m, unsigned long now) {
  uint16_t ink = k.eye;
  switch (m) {
    case MOOD_DONE:      // happy ^
      canvas.fillRect(ex,         ey + u, u, u, ink);
      canvas.fillRect(ex + u,     ey,     u, u, ink);
      canvas.fillRect(ex + 2 * u, ey + u, u, u, ink);
      break;
    case MOOD_ERROR:     // X
      for (uint8_t i = 0; i < 3; i++) {
        canvas.fillRect(ex + i * u, ey + i * u,       u, u, ink);
        canvas.fillRect(ex + i * u, ey + (2 - i) * u, u, u, ink);
      }
      break;
    default: {
      // Blink every ~6s for idle, otherwise eyes stay open
      bool blink = m == MOOD_IDLE && (now % 6000) < 200;
      if (k.eyes == EYES_SLASH && !blink) {
        canvas.fillRect(ex + u, ey,     u, 2 * u, ink);
        canvas.fillRect(ex,     ey + u, u, 2 * u, ink);
        break;
      }
      int16_t h = blink ? u / 2 + 1 : 2 * u;
      canvas.fillRect(ex, ey + (blink ? u : 0), 2 * u, h, ink);
    }
  }
}

// Creature grid is 12 wide x 9 tall; (x, feetY) is its bottom-left corner.
void drawCreature(const char* src, uint8_t color, Mood mood, unsigned long sinceMs, bool tinted,
                  int16_t x, int16_t feetY, uint8_t u, unsigned long now) {
  const Skin& k = skinFor(src);
  uint16_t body = tinted ? T->pal.state[MOOD_WAITING] : strcmp(k.src, src) ? T->pal.fallback[color] : k.body;
  unsigned long t = now - sinceMs;
  uint8_t lift = max(u, (uint8_t)2);   // a 1px hop is invisible on a follower

  if (mood == MOOD_SLEEPING) {     // squashed flat, eyes shut
    drawTorso(k, dimColor(body), dimColor(k.accent), dimColor(k.face), x, feetY - 4 * u, u, 2);
    if (k.knotHat) drawKnot(x + 6 * u - 5 * u / 2, feetY - 9 * u + u / 2, 5 * u, dimColor(body));
    uint16_t lid = dimColor(k.eye);
    int16_t ey = feetY - 3 * u + u / 4;
    canvas.fillRect(x + k.eyeL * u, ey, 2 * u, u / 2 + 1, lid);
    if (k.eyeR >= 0) canvas.fillRect(x + k.eyeR * u, ey, 2 * u, u / 2 + 1, lid);
    return;
  }

  int16_t dx = 0, dy = 0;
  switch (mood) {
    case MOOD_IDLE: {
      const int8_t wander[] = {0, -1, 0, 1};
      dx = wander[(now / 800) % 4] * u / 2;
      break;
    }
    case MOOD_WORKING: dy = -((now / 150) % 2) * (u / 3 + 1); break;
    case MOOD_WAITING: dy = -((now / 250) % 2) * lift; break;
    case MOOD_DONE:    dy = t < 1500 ? -((now / 150) % 2) * lift : 0; break;
    case MOOD_ERROR:   dx = (((now / 80) * 17) % 3 - 1) * (u / 3 + 1); break;
    default: break;
  }
  int16_t bx = x + dx, by = feetY - 9 * u + dy;

  // Jumps lift the legs off the ground; otherwise they stretch down to it
  bool up = mood == MOOD_WAITING || mood == MOOD_DONE;
  int16_t legY = by + 7 * u;
  int16_t legH = up ? 2 * u : feetY - legY;
  canvas.fillRect(bx + k.legL * u, legY, k.legW * u, legH, body);
  canvas.fillRect(bx + k.legR * u, legY, k.legW * u, legH, body);
  drawTorso(k, body, k.accent, k.face, bx, by, u, 1);
  if (k.knotHat) drawKnot(bx + 6 * u - 5 * u / 2, by - 5 * u + u / 2, 5 * u, body);

  // Arms: typing alternates, waiting and done throw them up
  int16_t lArm = 3, rArm = 3;
  if (mood == MOOD_WORKING) { bool f = (now / 150) % 2; lArm = f ? 2 : 4; rArm = f ? 4 : 2; }
  if (up) { lArm = 1; rArm = 1; }
  if (k.armL >= 0) canvas.fillRect(bx + k.armL * u, by + lArm * u, u, 2 * u, body);
  if (k.armR >= 0) canvas.fillRect(bx + k.armR * u, by + rArm * u, u, 2 * u, body);

  // Eyes: thinking looks around
  const int8_t look[] = {0, -1, 1, 0};
  int16_t e = mood == MOOD_THINKING ? look[(now / 600) % 4] * max(u / 2, 1) : 0;
  int16_t ly = by + k.eyeY * u, lx = bx + k.eyeL * u + e;
  drawEye(k, lx, ly, u, mood, now);
  if (k.eyeR >= 0) drawEye(k, bx + k.eyeR * u + e, ly, u, mood, now);

  if (mood == MOOD_DONE && t < 1500) {   // confetti
    for (uint8_t i = 0; i < 6; i++) {
      int16_t cx = x + (i * 37 + now / 40) % (12 * u);
      int16_t cy = by + ((i * 53 + now / 15) % (4 * u));
      canvas.fillRect(cx, cy, u / 2 + 1, u / 2 + 1, T->pal.confetti[i % 6]);
    }
  }
  if (mood == MOOD_ERROR) {              // smoke
    int16_t sy = by - ((now / 120) % 3) * u / 2;
    canvas.fillRect(bx + 10 * u, sy, u, u, T->pal.smoke);
    canvas.fillRect(bx + 11 * u, sy - u, u / 2 + 1, u / 2 + 1, T->pal.smoke);
  }
}

// Subagents line up above the main creature, oldest first. Each animates a beat
// behind the one before it so the row ripples instead of moving in lockstep.
void drawFollowers(const Session& s, int16_t x, int16_t w, int16_t feetY, uint8_t fu, unsigned long now) {
  uint8_t n = s.followerCount;
  if (!n) return;
  const int16_t pitch = 12 * fu + 1, room = w - 6, badgeW = 14;
  uint8_t shown = n * pitch <= room ? n : (room - badgeW) / pitch;
  uint8_t extra = n - shown;
  int16_t left = x + (w - shown * pitch - (extra ? badgeW : 0)) / 2 + 1;
  for (uint8_t i = 0; i < shown; i++) {
    const Follower& f = s.followers[i];
    unsigned long age = now - f.bornMs;
    int16_t drop = age < FOLLOWER_DROP_MS ? (FOLLOWER_DROP_MS - age) / 40 : 0;
    drawCreature(s.src, s.color, f.mood, f.moodSinceMs, f.mood == MOOD_WAITING,
                 left + i * pitch, feetY - drop, fu, now + i * 110);
  }
  if (extra) {
    canvas.setTextSize(1);
    canvas.setTextColor(T->pal.more);
    canvas.setCursor(left + shown * pitch + 1, feetY - 8);
    canvas.print('+');
    canvas.print(extra);
  }
}

const uint8_t* iconFor(Mood m, unsigned long now) {
  switch (m) {
    case MOOD_THINKING: return (now / 300) % 2 ? T->icons.gearA : T->icons.gearB;
    case MOOD_WORKING:  return (now / 200) % 2 ? T->icons.keysA : T->icons.keysB;
    case MOOD_WAITING:  return T->icons.ask;
    case MOOD_DONE:     return T->icons.check;
    case MOOD_ERROR:    return T->icons.cross;
    case MOOD_SLEEPING: return T->icons.zzz;
    default:            return nullptr;
  }
}

const char* labelOf(const Session& s) { return s.title[0] ? s.title : s.name; }

int16_t fitLabel(char* out, size_t size, const char* text, int16_t maxW) {
  strlcpy(out, text, size);
  size_t len = strlen(out);
  while (len && labelFont.getUTF8Width(out) > maxW) {
    do { len--; } while (len && (out[len] & 0xC0) == 0x80);
    out[len] = 0;
  }
  return labelFont.getUTF8Width(out);
}

int16_t drawLabel(const char* text, int16_t x, int16_t baselineY, int16_t maxW, uint16_t col) {
  char label[sizeof(Session::title)];
  int16_t w = fitLabel(label, sizeof(label), text, maxW);
  labelFont.setForegroundColor(col);
  labelFont.drawUTF8(x, baselineY, label);
  return w;
}

void drawCell(const Session& s, int16_t x, int16_t y, int16_t w, int16_t h, unsigned long now) {
  const Palette& P = T->pal;
  if (T->ops.cellBg) {
    T->ops.cellBg(x, y, w, h, s.mood, now);
  } else {
    canvas.fillRect(x, y, w, h, (s.mood == MOOD_WAITING && (now / 500) % 2) ? P.waitFlash : P.bg);
  }

  const int16_t labelH = 15, tileH = 6;
  int16_t tileY = y + h - labelH - tileH;

  // Largest creature that fits above the tile together with its icon
  uint8_t u = 2, iu = 2;
  for (uint8_t t = 12; t >= 2; t--) {
    uint8_t ti = t / 2 < 2 ? 2 : t / 2;
    if (12 * t <= w - 8 && 10 * t + 9 * ti + 2 <= tileY - y) { u = t; iu = ti; break; }
  }

  if (T->ops.tile) {
    T->ops.tile(x + 4, tileY, w - 8, tileH, s.mood, now);
  } else {
    canvas.fillRect(x + 4, tileY, w - 8, tileH, stateColor(s.mood));
    canvas.drawRect(x + 4, tileY, w - 8, tileH, P.tileBorder);
  }

  drawCreature(s.src, s.color, s.mood, s.moodSinceMs, false, x + (w - 12 * u) / 2, tileY, u, now);
  drawFollowers(s, x, w, tileY - 9 * u - (u / 3 + 4), u >= 11 ? 2 : 1, now);

  const uint8_t* icon = iconFor(s.mood, now);
  if (icon) {
    drawIcon(x + w - 8 * iu - 1, y + iu + 1, icon, iu, stateColor(s.mood));
  }

  // Session title (directory until one exists), cut on a UTF-8 boundary to fit
  char label[sizeof(s.title)];
  int16_t labelW = fitLabel(label, sizeof(label), labelOf(s), w - 4);
  labelFont.setForegroundColor(P.text);
  labelFont.drawUTF8(x + (w - labelW) / 2, tileY + tileH + 12, label);

  if (s.src[0]) {
    canvas.setTextSize(1);
    canvas.setTextColor(P.tag);
    canvas.setCursor(x + 3, y + 3);
    canvas.print(s.src);
  }
  if (s.mood == MOOD_ERROR) {
    canvas.drawRect(x, y, w, h, P.errorFrame);
    canvas.drawRect(x + 1, y + 1, w - 2, h - 2, P.errorFrame);
  }
}

void drawLabelCentered(const char* text, int16_t baselineY, uint16_t col) {
  labelFont.setForegroundColor(col);
  labelFont.drawUTF8((DISP_W - labelFont.getUTF8Width(text)) / 2, baselineY, text);
}

void drawPage(uint8_t page, int16_t ox, int16_t contentH, unsigned long now) {
  uint8_t first = page * T->perPage;
  uint8_t n = sessionCount > first ? min(sessionCount - first, (int)T->perPage) : 0;
  if (T->ops.page) {
    T->ops.page(first, n, ox, contentH, now);
    return;
  }
  const int16_t half = (DISP_W - 2) / 2, right = half + 2;
  const int16_t halfH = (contentH - 2) / 2, lower = contentH - halfH;

  canvas.fillRect(ox, 0, DISP_W, contentH, T->pal.gap);
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
    default: {   // a theme with more per page: 3 columns up to 6, then 4, rows as needed
      if (n < 5) break;
      const int16_t cols = n <= 6 ? 3 : 4, rows = (n + cols - 1) / cols;
      const int16_t w = (DISP_W - 2 * (cols - 1)) / cols, h = (contentH - 2 * (rows - 1)) / rows;
      for (uint8_t i = 0; i < n; i++) {
        drawCell(s[i], ox + (i % cols) * (w + 2), (i / cols) * (h + 2), w, h, now);
      }
    }
  }
}

// Fills rows [y, y+h); the page blocks sit in the bottom PAGER_H rows
void drawPager(uint8_t pages, uint8_t active, int16_t y, int16_t h, unsigned long now) {
  canvas.fillRect(0, y, DISP_W, h, T->pal.pagerBg);
  const int16_t dotW = 16, gap = 6;
  int16_t x = (DISP_W - pages * dotW - (pages - 1) * gap) / 2;
  for (uint8_t p = 0; p < pages; p++, x += dotW + gap) {
    uint16_t col = p == active ? T->pal.pagerOn : T->pal.pagerOff;
    // Pages holding an urgent session blink in that state's colour
    for (uint8_t i = p * T->perPage; i < sessionCount && i < (p + 1) * T->perPage; i++) {
      if (isAlerting(sessions[i]) && (now / 250) % 2) col = stateColor(sessions[i].mood);
    }
    canvas.fillRect(x, DISP_H - PAGER_H, dotW, PAGER_H, col);
  }
}

void drawToast() {
  const int16_t h = 20, y = DISP_H - PAGER_H - h - 4;
  canvas.fillRect(0, y, DISP_W, h, T->pal.toastBg);
  canvas.drawFastHLine(0, y, DISP_W, T->pal.toastText);
  drawLabelCentered(toast, y + 15, T->pal.toastText);
}

#ifdef CLAWD_PROFILE
// Compile with -DCLAWD_PROFILE to measure on the device: every 5 s, frame cost in microseconds.
// late = frames that took longer than their slot, so slides that run under 50 ms show up here.
#define PROFILE_EVERY_MS 5000UL
unsigned long profileSinceMs = 0, profileFrames = 0, profileLate = 0;
unsigned long profileRenderSum = 0, profilePushSum = 0, profileRenderMax = 0, profilePushMax = 0;

void profileFrame(unsigned long renderUs, unsigned long pushUs, unsigned long slotMs) {
  profileFrames++;
  profileRenderSum += renderUs;
  profilePushSum += pushUs;
  profileRenderMax = max(profileRenderMax, renderUs);
  profilePushMax = max(profilePushMax, pushUs);
  if (renderUs + pushUs > slotMs * 1000) profileLate++;
  if (millis() - profileSinceMs < PROFILE_EVERY_MS) return;
  Serial.printf("[profile] %s frames=%lu late=%lu render avg=%lu max=%lu push avg=%lu max=%lu"
                " heap free=%lu min=%lu ble=%d\n", T->key,
                profileFrames, profileLate, profileRenderSum / profileFrames, profileRenderMax,
                profilePushSum / profileFrames, profilePushMax, (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getMinFreeHeap(), bleConnected());
  profileSinceMs = millis();
  profileFrames = profileLate = profileRenderSum = profilePushSum = profileRenderMax = profilePushMax = 0;
}
#endif

void render() {
#ifdef CLAWD_PROFILE
  unsigned long renderStartUs = micros();
#endif
  unsigned long now = millis();
  uint8_t pages = pageCount();
  int16_t pagerH = T->ops.pagerHeight ? T->ops.pagerHeight(pages) : pages > 1 ? PAGER_H : 0;
  int16_t contentH = DISP_H - pagerH;

  if (sessionCount == 0 && T->ops.idleScreen) {
    T->ops.idleScreen(now);
  } else if (sessionCount == 0) {    // nobody connected: one sleeping creature
    Session none = {};
    strlcpy(none.name, "no session", sizeof(none.name));
    none.mood = MOOD_SLEEPING;
    drawCell(none, 0, 0, DISP_W, DISP_H, now);
  } else if (slideTo >= 0 && T->ops.transition) {
    T->ops.transition(currentPage, slideTo, slideProgress(now), contentH, now);
  } else if (slideTo >= 0) {
    uint8_t frame = min((now - slideStartMs) / SLIDE_FRAME_MS + 1, (unsigned long)SLIDE_FRAMES);
    int16_t shift = DISP_W * frame / SLIDE_FRAMES;
    drawPage(currentPage, -shift, contentH, now);
    drawPage(slideTo, DISP_W - shift, contentH, now);
  } else {
    drawPage(currentPage, 0, contentH, now);
  }
  if (T->ops.overlay) T->ops.overlay(now);
  uint8_t active = slideTo >= 0 ? slideTo : currentPage;
  if (sessionCount && pagerH > 0) {
    if (T->ops.pager) T->ops.pager(pages, active, contentH, pagerH, now);
    else drawPager(pages, active, contentH, pagerH, now);
  }
  if (toast[0]) {
    if ((long)(toastUntilMs - now) <= 0) toast[0] = 0;
    else if (T->ops.toast) T->ops.toast(toast, now);
    else drawToast();
  }

#ifdef CLAWD_PROFILE
  unsigned long pushStartUs = micros();
#endif
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), DISP_W, DISP_H);
#ifdef CLAWD_PROFILE
  profileFrame(pushStartUs - renderStartUs, micros() - pushStartUs, slideTo >= 0 ? SLIDE_FRAME_MS : FRAME_MS);
#endif
}

void setup() {
  Serial.setRxBufferSize(SERIAL_RX_BYTES);
  Serial.begin(115200);
  loadSettings();
  beginBacklight();
  SPI.begin(8, -1, 10, TFT_CS);   // SCK=8, MOSI=10 (ESP32-C3 pin remap)
  tft.setSPISpeed(40000000);
  tft.init(DISP_W, DISP_H);
  tft.setRotation(rotation);
  canvas.setTextWrap(false);      // a tag past the right edge mid-slide would land on the left one
  labelFont.begin(canvas);
  labelFont.setFont(u8g2_font_wqy12_t_gb2312b);  // resets font mode, so set it first
  labelFont.setFontMode(1);
  if (!canvas.getBuffer()) Serial.println("[warn] no RAM for frame buffer");
  bleBegin();   // after the frame buffer: it needs 115 KB in one piece, the radio doesn't
}

void loop() {
  pollSerial();
  pollBle();
  tickSessions();
  tickPaging();
  tickSettingsSave();
  unsigned long now = millis();
  unsigned long frameMs = slideTo >= 0 ? SLIDE_FRAME_MS : FRAME_MS;
  if (canvas.getBuffer() && now - lastFrameMs >= frameMs) {
    lastFrameMs = now;
    render();
  }
  delay(5);
}
