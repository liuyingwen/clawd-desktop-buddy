// Everything a theme may change, and everything of the sketch a theme may read.
// Adding one takes one theme_*.cpp and one entry in THEME_TABLE.
#pragma once
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>

// Ids are append-only and never reused: {"cfg":{"theme":N}} and NVS carry them. THEMES[0] is theme 1.
#define THEME_TABLE(X) X(THEME_CLASSIC) X(THEME_NOIR) X(THEME_PHOSPHOR) X(THEME_ARCADE) X(THEME_STAGE) \
                        X(THEME_METRO) X(THEME_TOWER) X(THEME_AQUARIUM)

#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

#define DISP_W 240
#define DISP_H 240

#define MAX_SESSIONS   12
#define MAX_FOLLOWERS  12                 // subagents remembered per session
#define PAGE_ROTATE_MS 5000UL             // a page's hold when Theme.pageMs is 0
#define SLIDE_FRAMES   6                  // a page change: 6 frames, 50 ms apart
#define SLIDE_FRAME_MS 50UL
#define FRAME_MS       100UL              // every other frame
#define PAGER_H        4                  // the built-in pager strip
#define TRANSITION_ONE 256                // progress of a finished page change

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
#define SKIN_COUNT 7      // same agents in the same order in every theme

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
// Hooks draw on `canvas`. Prefer pure functions of `now` and the session table: previews must loop,
// and a theme switch then leaves nothing behind. State a theme keeps anyway is reset in enter.
struct ThemeOps {
  void (*cellBg)(int16_t x, int16_t y, int16_t w, int16_t h, Mood m, unsigned long now);
  void (*tile)(int16_t x, int16_t y, int16_t w, int16_t h, Mood m, unsigned long now);
  void (*idleScreen)(unsigned long now);   // nobody connected
  void (*overlay)(unsigned long now);      // after the page, before the pager
  // Sessions first..first+count-1 (count <= perPage) in x [ox, ox+DISP_W), y [0, contentH). Fills its own background.
  // Null: the built-in grid of drawCell (1/2/3/4 split, a 3- or 4-column grid above 4).
  void (*page)(uint8_t first, uint8_t count, int16_t ox, int16_t contentH, unsigned long now);
  // The change from page `from` to page `to`, progress 1..TRANSITION_ONE over SLIDE_FRAMES frames.
  // Draws the whole content area; drawPage() draws either page. Null: the built-in slide to the left.
  void (*transition)(uint8_t from, uint8_t to, uint16_t progress, int16_t contentH, unsigned long now);
  // Rows the pager takes at the bottom, given the page count. Null: PAGER_H with more than one page, else 0.
  int16_t (*pagerHeight)(uint8_t pages);
  // Draws rows [y, y+h) whenever a session exists and h > 0. active is the page shown or being changed to.
  // Null: the built-in pager, which fills [y, y+h) and puts its page blocks in the bottom PAGER_H rows.
  void (*pager)(uint8_t pages, uint8_t active, int16_t y, int16_t h, unsigned long now);
  void (*toast)(const char* text, unsigned long now);   // settings confirmation, 3 s. Null: a band above the pager
  void (*enter)(unsigned long now);   // T just became this theme (boot, cfg): reset private state. Must not draw
};

// Page previews only (render.py); the device never reads it. 0 = the scene file's value, 10 fps.
// fps is 20, 10, 5, 4, 2 or 1; each preview frame is the frame on screen at that instant.
struct PreviewLoop {
  uint16_t settleMs, loopMs;
  uint8_t fps;
};
enum { PREVIEW_SQUAD, PREVIEW_SOLO, PREVIEW_EMPTY, PREVIEW_COUNT };

struct Theme {
  uint8_t id;
  const char* key;
  const char* name;        // shown on the device and the page; at most 8 Chinese characters
  const char* tagline;     // the rest of this line is only for the page's manifest
  uint32_t glow;           // 0xRRGGBB
  uint8_t minFw;           // firmware build that first shipped this theme
  uint8_t perPage;         // sessions per page, 1..MAX_SESSIONS; MAX_SESSIONS means one page, no paging
  uint16_t pageMs;         // a page's hold before the next page change; 0 = PAGE_ROTATE_MS.
                           // On the host's 5 ms loop a full page cycle is pageMs + 305 ms.
  Palette pal;
  const Skin* skins;
  const uint32_t* knot;    // Codex hat, 30x30, one word per row, bit 29 is the leftmost pixel
  IconSet icons;
  ThemeOps ops;
  PreviewLoop preview[PREVIEW_COUNT];   // squad, solo, empty
};

#define DECLARE_THEME(t) extern const Theme t;
THEME_TABLE(DECLARE_THEME)
#undef DECLARE_THEME

extern const uint32_t KNOT_LOGO[30];

// ── The squad, owned and written by the sketch. Themes only read it. ──

struct Follower {          // a subagent above its session's creature
  char id[7];
  Mood mood;
  bool leaving;            // SubagentStop came: celebrating, removed after FOLLOWER_LEAVE_MS
  bool betweenTools;
  unsigned long bornMs, moodSinceMs, lastEventMs;
};

struct Session {
  char id[9];
  char name[65];    // working directory, UTF-8
  char title[65];   // session title, kept once known
  char src[3];      // "CC", "CX", "OC", "PI", "DS", "WB", "CU" or another agent's tag, may be empty
  Mood mood;        // what the screen shows: own, or waiting for a follower
  Mood own;         // the main agent's state
  unsigned long ownSinceMs;
  Mood beforeCompact;  // own when compaction began, MOOD_UNKNOWN when not compacting
  Follower followers[MAX_FOLLOWERS];
  uint8_t followerCount;
  uint8_t color;    // index into Palette.fallback for an unknown agent
  unsigned long lastEventMs;
  bool quiet;                              // urgent state that shouldn't jump pages
  unsigned long moodSinceMs;
};

extern GFXcanvas16 canvas;                 // the frame, DISP_W x DISP_H, pushed whole every frame
extern const Theme* T;                     // the theme drawing now
extern U8G2_FOR_ADAFRUIT_GFX labelFont;    // wqy12 gb2312b (GB2312 level 1 + punctuation), transparent; don't change its font
extern Session sessions[MAX_SESSIONS];     // in arrival order; positions are stable
extern uint8_t sessionCount;
extern uint8_t currentPage;                // the page shown, or being left while slideTo >= 0
extern int8_t slideTo;                     // the page being changed to, -1 when no page change is on
extern unsigned long slideStartMs;         // when the page change began

inline bool isUrgent(Mood m) { return m == MOOD_WAITING || m == MOOD_ERROR; }   // waiting or error
bool isAlerting(const Session& s);         // urgent and not quiet: the states that jump pages and blink the pager
int8_t heldUrgent(unsigned long now);      // index of the urgent session the screen is holding on, -1 if none
uint8_t pageCount();                       // pages at T->perPage, at least 1
uint16_t slideProgress(unsigned long now); // 1..TRANSITION_ONE while slideTo >= 0, 0 otherwise
const char* labelOf(const Session& s);     // title, or the directory until a title is known
uint16_t stateColor(Mood m);               // T's colour for a state; MOOD_UNKNOWN reads as idle
uint16_t dimColor(uint16_t c);             // halfway to T->pal.sleepTarget, as sleeping creatures are
const uint8_t* iconFor(Mood m, unsigned long now);   // T's 7x7 state icon, animated; null for idle
const Skin& skinFor(const char* src);      // T's skin for an agent tag; skins[0] for an unknown one
// 7x7 icon at unit u, on a T->pal.plate square from (x-u, y-u) sized 9u
void drawIcon(int16_t x, int16_t y, const uint8_t* rows, uint8_t u, uint16_t col);
// 12x9-unit creature, bottom-left at (x, feetY); color: Session.color; tinted: body in the waiting colour
void drawCreature(const char* src, uint8_t color, Mood mood, unsigned long sinceMs, bool tinted,
                  int16_t x, int16_t feetY, uint8_t u, unsigned long now);
// A session's followers in one row centred in [x, x+w), feet at feetY, unit fu; "+N" when they don't fit
void drawFollowers(const Session& s, int16_t x, int16_t w, int16_t feetY, uint8_t fu, unsigned long now);
// Copies text into out, cut on a UTF-8 boundary to at most maxW pixels in labelFont; returns its width
int16_t fitLabel(char* out, size_t size, const char* text, int16_t maxW);
// Left-aligned labelFont text, cut to maxW pixels; returns the width drawn
int16_t drawLabel(const char* text, int16_t x, int16_t baselineY, int16_t maxW, uint16_t col);
void drawLabelCentered(const char* text, int16_t baselineY, uint16_t col);   // centred on the screen, uncut
// The built-in tile of one session: background, creature, followers, icon, label, agent tag
void drawCell(const Session& s, int16_t x, int16_t y, int16_t w, int16_t h, unsigned long now);
// Page `page` at x offset ox: T->ops.page, or the built-in grid
void drawPage(uint8_t page, int16_t ox, int16_t contentH, unsigned long now);
