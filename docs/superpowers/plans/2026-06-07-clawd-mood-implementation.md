# clawd-mood Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an ESP32-C3 desk mascot whose ST7789 TFT shows one of 7 pixel-eye expressions in real time, driven by Claude Code state events delivered over USB serial.

**Architecture:** A Claude Code plugin (hooks.json + hook.sh) translates events to single-state JSON lines and writes them to a named FIFO. A long-running uv-based Python daemon reads the FIFO and forwards lines to the ESP32 over USB CDC. The Arduino firmware parses NDJSON, runs a state machine (with Done-3s and Sleeping-5min timers), and renders pixel eyes.

**Tech Stack:** ESP32-C3 (Arduino IDE + Adafruit_GFX + Adafruit_ST7789 + ArduinoJson); macOS (uv + pyserial); Bash + jq.

**Reference projects (read but do NOT depend on as libraries):**
- `../projects/clawd-mochi/clawd_mochi.ino` — donor for ST7789 init + eye drawing primitives
- `../projects/claudigotchi/plugin/scripts/daemon.py` and `hook.sh` — pattern for FIFO + serial daemon

---

## File Structure

| Path | Responsibility |
|---|---|
| `firmware/clawd_mood/clawd_mood.ino` | Single Arduino sketch: pins, drawing, state machine, serial parsing |
| `plugin/.claude-plugin/plugin.json` | Plugin manifest |
| `plugin/hooks/hooks.json` | 9 Claude Code hook registrations |
| `plugin/scripts/hook.sh` | Maps each event name to a state JSON, writes to FIFO |
| `plugin/scripts/daemon.py` | Long-running serial bridge with PEP 723 inline deps |
| `README.md` | User docs |
| `.gitignore` | (already exists) |
| `docs/superpowers/specs/2026-06-07-clawd-mood-design.md` | (already exists) |

---

## Task 1: README skeleton

**Files:**
- Create: `README.md`

- [ ] **Step 1: Write README skeleton**

```markdown
# clawd-mood

ESP32-C3 desk mascot that reflects Claude Code state on a 1.54" pixel-eye TFT, over USB serial.

Built on top of [clawd-mochi](../projects/clawd-mochi/) hardware with [claudigotchi](../projects/claudigotchi/)-style plugin architecture.

## Status

🚧 In development. See `docs/superpowers/specs/` for the design spec.

## Setup

(filled in by later tasks)

## License

MIT
```

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "docs: README skeleton"
```

---

## Task 2: Firmware skeleton — boot to "READY" text

**Files:**
- Create: `firmware/clawd_mood/clawd_mood.ino`

Goal: get the sketch compiling and showing static text on the screen. No state machine yet — just verify the toolchain end-to-end before adding logic.

- [ ] **Step 1: Write minimal sketch**

```cpp
/*
 * clawd-mood — ESP32-C3 Super Mini + ST7789 1.54" 240x240
 *
 * Pins (same as clawd-mochi):
 *   SDA=GPIO10  SCL=GPIO8  RST=GPIO2  DC=GPIO1  CS=GPIO4  BL=GPIO3
 *   VCC=3V3  GND=GND
 *
 * Arduino IDE: ESP32C3 Dev Module, USB CDC On Boot: Enabled, 160 MHz, 921600.
 */

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

#define TFT_CS  4
#define TFT_DC  1
#define TFT_RST 2
#define TFT_BLK 3

#define DISP_W 240
#define DISP_H 240

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

void setup() {
  Serial.begin(115200);

  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);

  tft.init(DISP_W, DISP_H);
  tft.setRotation(2);
  tft.fillScreen(ST77XX_BLACK);

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(3);
  tft.setCursor(60, 100);
  tft.print("READY");
}

void loop() {
  delay(1000);
}
```

- [ ] **Step 2: Compile and upload via Arduino IDE**

In Arduino IDE: open `firmware/clawd_mood/clawd_mood.ino`. Tools → Board → ESP32C3 Dev Module, **USB CDC On Boot: Enabled**, CPU Frequency 160 MHz, Upload Speed 921600. Select the correct port, click Upload.

Expected: screen shows "READY" in white on black background.

- [ ] **Step 3: Commit**

```bash
git add firmware/clawd_mood/clawd_mood.ino
git commit -m "feat(firmware): minimal sketch boots and displays READY"
```

---

## Task 3: Firmware — serial line buffer + JSON parsing + state enum

Goal: receive `{"state":"working"}` over USB serial, parse it, store a `currentMood` enum, render the state name as text. No pixel eyes yet — text proves the state pipeline works.

**Files:**
- Modify: `firmware/clawd_mood/clawd_mood.ino`

**Dependencies:** install **ArduinoJson** via Library Manager.

- [ ] **Step 1: Replace the sketch with state-plumbing version**

```cpp
/*
 * clawd-mood — ESP32-C3 Super Mini + ST7789 1.54" 240x240
 */
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <ArduinoJson.h>

#define TFT_CS  4
#define TFT_DC  1
#define TFT_RST 2
#define TFT_BLK 3

#define DISP_W 240
#define DISP_H 240

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

enum Mood {
  MOOD_IDLE, MOOD_THINKING, MOOD_WORKING, MOOD_WAITING,
  MOOD_DONE, MOOD_ERROR, MOOD_SLEEPING, MOOD_UNKNOWN
};

Mood currentMood = MOOD_IDLE;
unsigned long lastEventMs = 0;
unsigned long doneEnteredMs = 0;
bool moodDirty = true;
String serialBuf;

const char* moodName(Mood m) {
  switch (m) {
    case MOOD_IDLE:     return "idle";
    case MOOD_THINKING: return "thinking";
    case MOOD_WORKING:  return "working";
    case MOOD_WAITING:  return "waiting";
    case MOOD_DONE:     return "done";
    case MOOD_ERROR:    return "error";
    case MOOD_SLEEPING: return "sleeping";
    default:            return "unknown";
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

void setMood(Mood m) {
  if (m == MOOD_UNKNOWN || m == currentMood) return;
  currentMood = m;
  moodDirty = true;
  if (m == MOOD_DONE) doneEnteredMs = millis();
}

void handleLine(const String& line) {
  if (!line.length()) return;
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    Serial.print("[warn] bad json: "); Serial.println(line);
    return;
  }
  const char* s = doc["state"] | "";
  Mood next = parseMood(s);
  if (next == MOOD_UNKNOWN) {
    Serial.print("[warn] unknown state: "); Serial.println(s);
    return;
  }
  setMood(next);
  lastEventMs = millis();
}

void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      handleLine(serialBuf);
      serialBuf = "";
    } else if (c != '\r') {
      serialBuf += c;
      if (serialBuf.length() > 256) serialBuf = "";
    }
  }
}

void renderMoodText() {
  if (!moodDirty) return;
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(3);
  tft.setCursor(20, 100);
  tft.print(moodName(currentMood));
  moodDirty = false;
}

void setup() {
  Serial.begin(115200);
  pinMode(TFT_BLK, OUTPUT); digitalWrite(TFT_BLK, HIGH);
  tft.init(DISP_W, DISP_H);
  tft.setRotation(2);
  lastEventMs = millis();
}

void loop() {
  pollSerial();
  renderMoodText();
  delay(30);
}
```

- [ ] **Step 2: Compile, upload, test via serial monitor**

Open Arduino Serial Monitor at 115200 baud, line ending = Newline. Type each line and press send:

```
{"state":"working"}
{"state":"thinking"}
{"state":"done"}
{"state":"sleeping"}
{"state":"bogus"}
not json at all
```

Expected: screen shows `working` → `thinking` → `done` → `sleeping`. For the last two, screen stays on `sleeping` and serial monitor shows `[warn] unknown state: bogus` and `[warn] bad json: not json at all`.

- [ ] **Step 3: Commit**

```bash
git add firmware/clawd_mood/clawd_mood.ino
git commit -m "feat(firmware): serial-driven mood state machine (text rendering)"
```

---

## Task 4: Firmware — Done auto-revert (3s) + Sleeping auto-enter (5min)

Goal: add automatic transitions. Done becomes Idle after 3 seconds. Any mood becomes Sleeping after 5 minutes of no serial messages.

**Files:**
- Modify: `firmware/clawd_mood/clawd_mood.ino`

- [ ] **Step 1: Add timer constants near the top (after `#define DISP_H`)**

```cpp
#define DONE_REVERT_MS    3000UL          // Done → Idle after 3s
#define SLEEP_IDLE_MS     (5UL*60*1000)   // 5 minutes
```

- [ ] **Step 2: Add `tickMoodMachine()` function (place after `setMood`)**

```cpp
void tickMoodMachine() {
  unsigned long now = millis();
  if (currentMood == MOOD_DONE && now - doneEnteredMs > DONE_REVERT_MS) {
    setMood(MOOD_IDLE);
  }
  if (currentMood != MOOD_SLEEPING && now - lastEventMs > SLEEP_IDLE_MS) {
    setMood(MOOD_SLEEPING);
  }
}
```

- [ ] **Step 3: Call it from `loop()`**

Change `loop()` to:

```cpp
void loop() {
  pollSerial();
  tickMoodMachine();
  renderMoodText();
  delay(30);
}
```

- [ ] **Step 4: Upload and test**

Test Done → Idle:
1. Send `{"state":"done"}` — screen shows "done"
2. Wait 3 seconds — screen should change to "idle"

Test Sleeping auto-enter — for the test we'll temporarily lower the threshold:

Temporarily change `SLEEP_IDLE_MS` to `10*1000UL` (10 seconds), upload, send `{"state":"working"}`, wait 10s, screen should change to "sleeping". Then revert the constant back to 5 minutes and upload again.

- [ ] **Step 5: Commit**

```bash
git add firmware/clawd_mood/clawd_mood.ino
git commit -m "feat(firmware): auto-revert Done after 3s, auto-Sleep after 5min idle"
```

---

## Task 5: Firmware — port eye geometry and drawing primitives from clawd-mochi

Goal: bring over the eye constants, color palette, and `drawNormalEyes` / `drawSquishEyes` from `clawd-mochi/clawd_mochi.ino`. Don't wire them to the state machine yet — Task 6 does that.

**Reference:** `projects/clawd-mochi/clawd_mochi.ino` lines 40–61 (constants), 211–219 (`initColours`), 243–290 (eye drawing).

**Files:**
- Modify: `firmware/clawd_mood/clawd_mood.ino`

- [ ] **Step 1: Add geometry + color globals (after `#define DISP_H`)**

```cpp
// Eye geometry (shared with clawd-mochi)
#define EYE_W   30
#define EYE_H   60
#define EYE_GAP 120
#define EYE_OX  0
#define EYE_OY  40

// Colors
uint16_t C_ORANGE, C_DARKBG, C_MUTED;
#define C_WHITE ST77XX_WHITE
#define C_BLACK ST77XX_BLACK

uint16_t bgColor = 0;  // initialized in initColors()
```

- [ ] **Step 2: Add `initColors()` and eye geometry inlines**

```cpp
void initColors() {
  C_ORANGE = tft.color565(218, 17, 0);
  C_DARKBG = tft.color565(10, 12, 16);
  C_MUTED  = tft.color565(90, 88, 86);
  bgColor  = C_ORANGE;
}

inline int16_t eyeLX(int16_t ox) {
  return (DISP_W - (EYE_W * 2 + EYE_GAP)) / 2 + EYE_OX + ox;
}
inline int16_t eyeRX(int16_t ox) { return eyeLX(ox) + EYE_W + EYE_GAP; }
inline int16_t eyeY()            { return (DISP_H - EYE_H) / 2 - EYE_OY; }
inline int16_t eyeCY()           { return eyeY() + EYE_H / 2; }
```

- [ ] **Step 3: Add the two primitive draw functions**

```cpp
void drawNormalEyes(int16_t ox = 0, bool blink = false) {
  tft.fillScreen(bgColor);
  const int16_t lx = eyeLX(ox), rx = eyeRX(ox), ey = eyeY();
  if (!blink) {
    tft.fillRect(lx, ey, EYE_W, EYE_H, C_BLACK);
    tft.fillRect(rx, ey, EYE_W, EYE_H, C_BLACK);
  } else {
    tft.fillRect(lx, ey + EYE_H / 2 - 3, EYE_W, 6, C_BLACK);
    tft.fillRect(rx, ey + EYE_H / 2 - 3, EYE_W, 6, C_BLACK);
  }
}

void drawChevron(int16_t cx, int16_t cy, int16_t arm, int16_t reach,
                 uint8_t thk, bool rightFacing, uint16_t col) {
  for (int8_t t = -(int8_t)thk; t <= (int8_t)thk; t++) {
    if (rightFacing) {
      tft.drawLine(cx - reach/2, cy - arm + t, cx + reach/2, cy + t,      col);
      tft.drawLine(cx + reach/2, cy + t,       cx - reach/2, cy + arm + t, col);
    } else {
      tft.drawLine(cx + reach/2, cy - arm + t, cx - reach/2, cy + t,      col);
      tft.drawLine(cx - reach/2, cy + t,       cx + reach/2, cy + arm + t, col);
    }
  }
}

void drawSquishEyes(bool closed = false) {
  tft.fillScreen(bgColor);
  const int16_t lx = eyeLX(0), rx = eyeRX(0), cy = eyeCY();
  const int16_t arm   = EYE_H / 2;
  const int16_t reach = EYE_W / 2;
  const int16_t lcx   = lx + EYE_W / 2;
  const int16_t rcx   = rx + EYE_W / 2;
  if (!closed) {
    drawChevron(lcx, cy, arm, reach, 10, true,  C_BLACK);
    drawChevron(rcx, cy, arm, reach, 10, false, C_BLACK);
  } else {
    tft.fillRect(lx, cy - 5, EYE_W, 10, C_BLACK);
    tft.fillRect(rx, cy - 5, EYE_W, 10, C_BLACK);
  }
}
```

- [ ] **Step 4: Call `initColors()` in `setup()` after `tft.init`**

In `setup()`, after `tft.setRotation(2);`, add:

```cpp
  initColors();
```

- [ ] **Step 5: Compile-check only**

Don't change the renderer yet. Just confirm the sketch still compiles. Upload and confirm screen still shows the text-mode mood (no regression).

- [ ] **Step 6: Commit**

```bash
git add firmware/clawd_mood/clawd_mood.ino
git commit -m "feat(firmware): port eye geometry, colors, drawNormalEyes/drawSquishEyes"
```

---

## Task 6: Firmware — drawIdle (replaces text rendering)

Goal: replace `renderMoodText` with a per-mood dispatcher and implement `drawIdle` first. From this task on, every state will draw real eyes.

**Files:**
- Modify: `firmware/clawd_mood/clawd_mood.ino`

- [ ] **Step 1: Replace `renderMoodText` with `renderMood` + per-mood functions**

Delete `renderMoodText`. Add this block after `drawSquishEyes`:

```cpp
// ── Per-mood renderers ──────────────────────────────────────────
// Each renderer is called every frame (~33fps). They use a static
// frame counter for animation. They redraw only when needed.

void drawIdle() {
  static uint8_t step = 0;
  static unsigned long lastStep = 0;
  unsigned long now = millis();
  // Slow wiggle: every 800ms, cycle through 5 horizontal offsets
  if (moodDirty || now - lastStep > 800) {
    const int16_t offs[] = {0, -4, 0, 4, 0};
    // Blink every ~3.2s (every 4th step)
    bool blink = (step % 4 == 0) && (step != 0);
    drawNormalEyes(offs[step % 5], blink);
    step++;
    lastStep = now;
    moodDirty = false;
  }
}

void renderMood() {
  switch (currentMood) {
    case MOOD_IDLE:     drawIdle(); break;
    case MOOD_THINKING: drawNormalEyes(0); moodDirty = false; break;  // placeholder
    case MOOD_WORKING:  drawNormalEyes(0); moodDirty = false; break;  // placeholder
    case MOOD_WAITING:  drawNormalEyes(0); moodDirty = false; break;  // placeholder
    case MOOD_DONE:     drawSquishEyes(false); moodDirty = false; break;  // placeholder
    case MOOD_ERROR:    drawNormalEyes(0); moodDirty = false; break;  // placeholder
    case MOOD_SLEEPING: drawNormalEyes(0, true); moodDirty = false; break;  // placeholder
    default: break;
  }
}
```

- [ ] **Step 2: Wire `renderMood()` into `loop()`**

Change `loop()`'s call from `renderMoodText();` to `renderMood();`.

- [ ] **Step 3: Upload and test**

Send `{"state":"idle"}` — expect orange background, two black square eyes wiggling slowly side-to-side, blinking every ~3 seconds. Send other states — expect placeholder rendering (eyes or squish or closed lines).

- [ ] **Step 4: Commit**

```bash
git add firmware/clawd_mood/clawd_mood.ino
git commit -m "feat(firmware): drawIdle wiggling + blinking pixel eyes"
```

---

## Task 7: Firmware — drawThinking (eyes look around loop)

**Files:**
- Modify: `firmware/clawd_mood/clawd_mood.ino`

- [ ] **Step 1: Implement `drawThinking`**

Add after `drawIdle`:

```cpp
void drawThinking() {
  static uint8_t phase = 0;
  static unsigned long lastStep = 0;
  unsigned long now = millis();
  // Loop: up → left → right → center, 600ms each
  if (moodDirty || now - lastStep > 600) {
    tft.fillScreen(bgColor);
    int16_t lx = eyeLX(0), rx = eyeRX(0), ey = eyeY();
    int16_t dx = 0, dy = 0;
    switch (phase % 4) {
      case 0: dx = 0;  dy = -8; break; // up
      case 1: dx = -8; dy = 0;  break; // left
      case 2: dx = 8;  dy = 0;  break; // right
      case 3: dx = 0;  dy = 0;  break; // center
    }
    tft.fillRect(lx + dx, ey + dy, EYE_W, EYE_H, C_BLACK);
    tft.fillRect(rx + dx, ey + dy, EYE_W, EYE_H, C_BLACK);
    phase++;
    lastStep = now;
    moodDirty = false;
  }
}
```

- [ ] **Step 2: Wire into `renderMood()`**

Replace the `MOOD_THINKING` case with:

```cpp
case MOOD_THINKING: drawThinking(); break;
```

- [ ] **Step 3: Upload and test**

Send `{"state":"thinking"}`. Expect eyes cycling up → left → right → center every 600ms.

- [ ] **Step 4: Commit**

```bash
git add firmware/clawd_mood/clawd_mood.ino
git commit -m "feat(firmware): drawThinking eye-direction loop"
```

---

## Task 8: Firmware — drawWorking (focused eyes + bottom dots)

**Files:**
- Modify: `firmware/clawd_mood/clawd_mood.ino`

- [ ] **Step 1: Implement `drawWorking`**

Add after `drawThinking`:

```cpp
void drawWorking() {
  static uint8_t dots = 0;
  static unsigned long lastStep = 0;
  unsigned long now = millis();
  if (moodDirty || now - lastStep > 300) {
    tft.fillScreen(bgColor);
    int16_t lx = eyeLX(0), rx = eyeRX(0), ey = eyeY();
    // Slight focused twitch: 2px random jitter on each eye
    int16_t jx = (now / 100) % 3 - 1;  // -1, 0, 1
    tft.fillRect(lx + jx, ey, EYE_W, EYE_H, C_BLACK);
    tft.fillRect(rx - jx, ey, EYE_W, EYE_H, C_BLACK);
    // Bottom: . / .. / ... cycle
    tft.setTextColor(C_BLACK);
    tft.setTextSize(3);
    tft.setCursor(95, 180);
    if      (dots % 3 == 0) tft.print(".");
    else if (dots % 3 == 1) tft.print("..");
    else                    tft.print("...");
    dots++;
    lastStep = now;
    moodDirty = false;
  }
}
```

- [ ] **Step 2: Wire into `renderMood()`**

```cpp
case MOOD_WORKING: drawWorking(); break;
```

- [ ] **Step 3: Upload and test**

Send `{"state":"working"}`. Expect eyes with slight jitter and bottom dots cycling `.` → `..` → `...` every 300ms.

- [ ] **Step 4: Commit**

```bash
git add firmware/clawd_mood/clawd_mood.ino
git commit -m "feat(firmware): drawWorking jitter eyes + animated dots"
```

---

## Task 9: Firmware — drawWaiting (wide eyes bouncing + ?)

**Files:**
- Modify: `firmware/clawd_mood/clawd_mood.ino`

- [ ] **Step 1: Implement `drawWaiting`**

Add after `drawWorking`:

```cpp
void drawWaiting() {
  static uint8_t step = 0;
  static unsigned long lastStep = 0;
  unsigned long now = millis();
  if (moodDirty || now - lastStep > 250) {
    tft.fillScreen(bgColor);
    int16_t lx = eyeLX(0), rx = eyeRX(0), ey = eyeY();
    int16_t bounce = (step % 4 < 2) ? -6 : 6;
    // Slightly bigger eyes for "wide open" look
    int16_t eh = EYE_H + 6;
    tft.fillRect(lx, ey + bounce - 3, EYE_W, eh, C_BLACK);
    tft.fillRect(rx, ey + bounce - 3, EYE_W, eh, C_BLACK);
    tft.setTextColor(C_BLACK);
    tft.setTextSize(4);
    tft.setCursor(105, 185);
    tft.print("?");
    step++;
    lastStep = now;
    moodDirty = false;
  }
}
```

- [ ] **Step 2: Wire into `renderMood()`**

```cpp
case MOOD_WAITING: drawWaiting(); break;
```

- [ ] **Step 3: Upload and test**

Send `{"state":"waiting"}`. Expect wide eyes bouncing up/down every 250ms with a `?` underneath.

- [ ] **Step 4: Commit**

```bash
git add firmware/clawd_mood/clawd_mood.ino
git commit -m "feat(firmware): drawWaiting bouncing wide eyes with ?"
```

---

## Task 10: Firmware — drawDone (squish + 1s anim then steady)

**Files:**
- Modify: `firmware/clawd_mood/clawd_mood.ino`

- [ ] **Step 1: Implement `drawDone`**

Add after `drawWaiting`:

```cpp
void drawDone() {
  static unsigned long enteredAt = 0;
  static uint8_t phase = 0;
  unsigned long now = millis();
  if (moodDirty) {
    enteredAt = now;
    phase = 0;
    moodDirty = false;
  }
  unsigned long elapsed = now - enteredAt;
  // First 1000ms: alternate squish/closed every 150ms for celebration
  // After 1000ms: steady squish
  if (elapsed < 1000) {
    uint8_t newPhase = (elapsed / 150) % 2;
    if (newPhase != phase) {
      drawSquishEyes(newPhase == 1);
      phase = newPhase;
    }
  } else if (phase != 99) {
    drawSquishEyes(false);
    phase = 99;
  }
}
```

- [ ] **Step 2: Wire into `renderMood()`**

```cpp
case MOOD_DONE: drawDone(); break;
```

- [ ] **Step 3: Upload and test**

Send `{"state":"done"}`. Expect 1 second of alternating squish/closed-line eyes, then steady `> <` until the 3-second auto-revert kicks in and screen returns to idle wiggle.

- [ ] **Step 4: Commit**

```bash
git add firmware/clawd_mood/clawd_mood.ino
git commit -m "feat(firmware): drawDone celebratory squish then steady smile"
```

---

## Task 11: Firmware — drawError (skewed jittery eyes)

**Files:**
- Modify: `firmware/clawd_mood/clawd_mood.ino`

- [ ] **Step 1: Implement `drawError`**

Add after `drawDone`:

```cpp
void drawError() {
  static unsigned long lastStep = 0;
  unsigned long now = millis();
  if (moodDirty || now - lastStep > 80) {
    tft.fillScreen(bgColor);
    int16_t lx = eyeLX(0), rx = eyeRX(0), ey = eyeY();
    // Jittery, asymmetric: random small offset, left eye lower than right
    int16_t jx = ((now / 80) * 17) % 5 - 2;
    int16_t jy = ((now / 80) * 31) % 5 - 2;
    tft.fillRect(lx + jx, ey + 6 + jy, EYE_W, EYE_H - 6, C_BLACK);
    tft.fillRect(rx - jx, ey - 6 + jy, EYE_W, EYE_H - 6, C_BLACK);
    lastStep = now;
    moodDirty = false;
  }
}
```

- [ ] **Step 2: Wire into `renderMood()`**

```cpp
case MOOD_ERROR: drawError(); break;
```

- [ ] **Step 3: Upload and test**

Send `{"state":"error"}`. Expect jittery eyes at different vertical positions (left lower, right higher), shimmering every 80ms.

- [ ] **Step 4: Commit**

```bash
git add firmware/clawd_mood/clawd_mood.ino
git commit -m "feat(firmware): drawError asymmetric jittery eyes"
```

---

## Task 12: Firmware — drawSleeping (closed eyes + Z floating)

**Files:**
- Modify: `firmware/clawd_mood/clawd_mood.ino`

- [ ] **Step 1: Implement `drawSleeping`**

Add after `drawError`:

```cpp
void drawSleeping() {
  static unsigned long lastStep = 0;
  static uint8_t zPhase = 0;
  unsigned long now = millis();
  if (moodDirty || now - lastStep > 500) {
    tft.fillScreen(bgColor);
    int16_t lx = eyeLX(0), rx = eyeRX(0), cy = eyeCY();
    // Closed eyes: thin horizontal bars
    tft.fillRect(lx, cy - 2, EYE_W, 4, C_BLACK);
    tft.fillRect(rx, cy - 2, EYE_W, 4, C_BLACK);
    // Floating Z: 3 positions, rising
    tft.setTextColor(C_BLACK);
    tft.setTextSize(2);
    int16_t zy[] = {170, 150, 130};
    int16_t zx[] = {140, 150, 160};
    tft.setCursor(zx[zPhase % 3], zy[zPhase % 3]);
    tft.print("Z");
    zPhase++;
    lastStep = now;
    moodDirty = false;
  }
}
```

- [ ] **Step 2: Wire into `renderMood()`**

```cpp
case MOOD_SLEEPING: drawSleeping(); break;
```

- [ ] **Step 3: Upload and test**

Send `{"state":"sleeping"}`. Expect closed-line eyes with a `Z` letter floating upward (3 positions) and slightly right, redrawing every 500ms.

Confirm wake: send `{"state":"working"}` while sleeping — should immediately switch to working animation.

- [ ] **Step 4: Commit**

```bash
git add firmware/clawd_mood/clawd_mood.ino
git commit -m "feat(firmware): drawSleeping closed eyes with floating Z"
```

---

## Task 13: Firmware — full end-to-end serial smoke test

Goal: catch regressions by going through all 7 states in sequence via serial monitor, plus the two timers.

**Files:** none (test only).

- [ ] **Step 1: Manually verify all 7 states from Arduino Serial Monitor**

For each of the 7 lines below, send and visually confirm:

```
{"state":"idle"}      → orange background, eyes wiggle + blink
{"state":"thinking"}  → eyes cycle up/left/right/center
{"state":"working"}   → eyes jitter, dots cycle "." ".." "..."
{"state":"waiting"}   → wide eyes bounce, "?" below
{"state":"done"}      → squish smile, after 3s auto-revert to idle
{"state":"error"}     → asymmetric jittery eyes
{"state":"sleeping"}  → closed eyes, "Z" floats
```

- [ ] **Step 2: Verify Done auto-revert**

Send `{"state":"done"}`, count 3 seconds, verify display returns to idle wiggle.

- [ ] **Step 3: Verify Sleeping auto-enter (with temporary short threshold)**

Temporarily change `SLEEP_IDLE_MS` to `15000UL` (15 seconds), upload, send `{"state":"working"}`, wait 15s without sending anything else, verify display switches to sleeping. **Then revert the constant back to `(5UL*60*1000)` and re-upload.**

- [ ] **Step 4: Verify wake from Sleeping**

While in sleeping, send `{"state":"working"}` — should switch immediately.

- [ ] **Step 5: Commit (no code change, but record verification milestone)**

This step has no commit since no code changed; it's a verification gate before moving to the daemon.

---

## Task 14: Daemon — script skeleton with PEP 723 + serial open + port detect

**Files:**
- Create: `plugin/scripts/daemon.py`

- [ ] **Step 1: Write the daemon script**

```python
#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = ["pyserial>=3.5"]
# ///
"""clawd-mood daemon — keeps serial port open and forwards state from FIFO to ESP32."""

import glob
import json
import os
import signal
import sys
import time

import serial

FIFO_PATH = "/tmp/clawd-mood.fifo"
BAUD_RATE = 115200


def detect_port() -> str:
    override = os.environ.get("CLAWD_MOOD_PORT")
    if override:
        return override
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit(
            "No /dev/cu.usbmodem* device found. "
            "Is the ESP32 plugged in? Or set CLAWD_MOOD_PORT=/dev/cu.xxx"
        )
    if len(ports) > 1:
        print(f"  warning: multiple devices found {ports}, using {ports[0]}",
              file=sys.stderr)
    return ports[0]


def open_serial(port: str) -> serial.Serial:
    ser = serial.Serial(
        port, BAUD_RATE, timeout=1,
        dsrdtr=False, rtscts=False,
    )
    # Suppress reset on open
    ser.dtr = False
    ser.rts = False
    time.sleep(0.5)
    ser.read(1000)  # drain boot output
    time.sleep(1.0)
    ser.read(1000)
    return ser


def make_fifo() -> None:
    try:
        os.unlink(FIFO_PATH)
    except FileNotFoundError:
        pass
    os.mkfifo(FIFO_PATH)


def cleanup(*_args):
    try:
        os.unlink(FIFO_PATH)
    except FileNotFoundError:
        pass
    sys.exit(0)


def main() -> None:
    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    port = detect_port()
    make_fifo()
    print(f"clawd-mood daemon started")
    print(f"  FIFO:   {FIFO_PATH}")
    print(f"  Serial: {port}")

    ser = open_serial(port)
    print("  Ready!")

    while True:
        # Opening FIFO blocks until a writer connects
        with open(FIFO_PATH, "r") as fifo:
            for line in fifo:
                line = line.strip()
                if not line:
                    continue
                try:
                    json.loads(line)
                except json.JSONDecodeError:
                    print(f"  !! bad JSON: {line}", file=sys.stderr)
                    continue
                try:
                    ser.write((line + "\n").encode())
                    ser.flush()
                    print(f"  -> {line}")
                except serial.SerialException as e:
                    print(f"  !! serial error: {e}", file=sys.stderr)
                    sys.exit(1)


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Make executable**

```bash
chmod +x plugin/scripts/daemon.py
```

- [ ] **Step 3: Smoke test the daemon — ESP32 plugged in**

```bash
./plugin/scripts/daemon.py
```

Expected output:
```
clawd-mood daemon started
  FIFO:   /tmp/clawd-mood.fifo
  Serial: /dev/cu.usbmodem...
  Ready!
```

uv will auto-install pyserial into an isolated venv on first run.

In another terminal, feed the FIFO directly:

```bash
echo '{"state":"working"}' > /tmp/clawd-mood.fifo
echo '{"state":"done"}' > /tmp/clawd-mood.fifo
echo '{"state":"idle"}' > /tmp/clawd-mood.fifo
```

Daemon terminal should print:
```
  -> {"state":"working"}
  -> {"state":"done"}
  -> {"state":"idle"}
```

And the ESP32 display should switch through working → done → idle.

- [ ] **Step 4: Test bad-JSON tolerance**

```bash
echo 'not json' > /tmp/clawd-mood.fifo
```

Daemon should log `!! bad JSON: not json` to stderr but stay alive. Send a valid line afterwards to confirm.

- [ ] **Step 5: Test ESP32-missing case**

Unplug the ESP32. Stop daemon (Ctrl+C). Restart `./plugin/scripts/daemon.py`. Expected: exits with message `No /dev/cu.usbmodem* device found...`. Replug ESP32 and continue.

- [ ] **Step 6: Commit**

```bash
git add plugin/scripts/daemon.py
git commit -m "feat(daemon): uv-driven serial bridge with FIFO and port detect"
```

---

## Task 15: Plugin manifest + hooks registration

**Files:**
- Create: `plugin/.claude-plugin/plugin.json`
- Create: `plugin/hooks/hooks.json`

- [ ] **Step 1: Write plugin manifest**

```json
{
  "name": "clawd-mood",
  "description": "Drives a clawd-mood ESP32-C3 desk mascot via USB serial, reflecting Claude Code state on a TFT display.",
  "version": "0.1.0",
  "author": { "name": "liuyingwen" }
}
```

- [ ] **Step 2: Write hooks.json**

```json
{
  "hooks": {
    "SessionStart": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "\"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.sh", "async": true }
      ]}
    ],
    "UserPromptSubmit": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "\"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.sh", "async": true }
      ]}
    ],
    "PreToolUse": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "\"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.sh", "async": true }
      ]}
    ],
    "PostToolUse": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "\"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.sh", "async": true }
      ]}
    ],
    "PostToolUseFailure": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "\"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.sh", "async": true }
      ]}
    ],
    "Notification": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "\"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.sh", "async": true }
      ]}
    ],
    "Stop": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "\"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.sh", "async": true }
      ]}
    ],
    "SubagentStart": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "\"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.sh", "async": true }
      ]}
    ],
    "SubagentStop": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "\"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.sh", "async": true }
      ]}
    ]
  }
}
```

- [ ] **Step 3: Validate JSON syntactically**

```bash
python3 -m json.tool plugin/.claude-plugin/plugin.json > /dev/null
python3 -m json.tool plugin/hooks/hooks.json > /dev/null
```

Expected: no output (no errors).

- [ ] **Step 4: Commit**

```bash
git add plugin/.claude-plugin/plugin.json plugin/hooks/hooks.json
git commit -m "feat(plugin): manifest + 9 hook event registrations"
```

---

## Task 16: hook.sh — event → state JSON

**Files:**
- Create: `plugin/scripts/hook.sh`

- [ ] **Step 1: Write the hook script**

```bash
#!/bin/bash
# clawd-mood hook — maps Claude Code events to ESP32 states via FIFO.
# Receives hook JSON on stdin, writes state JSON to /tmp/clawd-mood.fifo.

FIFO="/tmp/clawd-mood.fifo"

# Exit silently if daemon isn't running or jq is missing.
[ -p "$FIFO" ] || exit 0
command -v jq >/dev/null || exit 0

INPUT=$(cat)
EVENT=$(echo "$INPUT" | jq -r '.hook_event_name // empty')
TOOL=$(echo "$INPUT" | jq -r '.tool_name // empty')

case "$EVENT" in
  SessionStart)       STATE="idle" ;;
  UserPromptSubmit)   STATE="thinking" ;;
  PreToolUse)         STATE="working" ;;
  PostToolUse)        STATE="working" ;;
  PostToolUseFailure) STATE="error" ;;
  Notification)       STATE="waiting" ;;
  Stop)               STATE="done" ;;
  SubagentStart)      STATE="working" ;;
  SubagentStop)       STATE="working" ;;
  *)                  exit 0 ;;
esac

# 1 second timeout protects against FIFO with no reader (daemon died mid-event).
timeout 1 bash -c "echo '{\"state\":\"$STATE\",\"event\":\"$EVENT\",\"tool\":\"$TOOL\"}' > '$FIFO'" || true
exit 0
```

- [ ] **Step 2: Make executable**

```bash
chmod +x plugin/scripts/hook.sh
```

- [ ] **Step 3: Test the script by piping a fake hook event into it**

Make sure the daemon is running. Then in another terminal:

```bash
echo '{"hook_event_name":"PreToolUse","tool_name":"Bash"}' | ./plugin/scripts/hook.sh
```

Daemon terminal should print:
```
  -> {"state":"working","event":"PreToolUse","tool":"Bash"}
```

ESP32 display should switch to working animation.

Test a few more events:

```bash
echo '{"hook_event_name":"Notification"}'      | ./plugin/scripts/hook.sh   # → waiting
echo '{"hook_event_name":"Stop"}'              | ./plugin/scripts/hook.sh   # → done (then idle 3s later)
echo '{"hook_event_name":"PostToolUseFailure"}'| ./plugin/scripts/hook.sh   # → error
echo '{"hook_event_name":"SomeUnknownEvent"}'  | ./plugin/scripts/hook.sh   # → no-op
```

- [ ] **Step 4: Test daemon-down safety**

Stop the daemon (Ctrl+C in its terminal). Then:

```bash
echo '{"hook_event_name":"PreToolUse"}' | ./plugin/scripts/hook.sh
echo $?
```

Expected: exits 0 with no output (FIFO doesn't exist, script bails silently).

- [ ] **Step 5: Commit**

```bash
git add plugin/scripts/hook.sh
git commit -m "feat(plugin): hook.sh maps events to state JSON via FIFO"
```

---

## Task 17: End-to-end integration test with real Claude Code

**Files:** none (verification only).

- [ ] **Step 1: Start the daemon**

```bash
./plugin/scripts/daemon.py
```

Leave it running. Display should be on idle/sleeping/whatever the firmware last had.

- [ ] **Step 2: Launch Claude Code with the plugin**

In a new terminal:

```bash
claude --plugin-dir /Users/liuyingwen/Documents/0.项目/0.aiwtf/2.aiwtf-claude-code-desktop/clawd-mood/plugin
```

- [ ] **Step 3: Verify hooks registered**

In the Claude Code session run:

```
/hooks
```

Expected: see entries for SessionStart, UserPromptSubmit, PreToolUse, PostToolUse, PostToolUseFailure, Notification, Stop, SubagentStart, SubagentStop — all pointing to `clawd-mood`.

ESP32 should already have switched to **thinking** as a side effect of `UserPromptSubmit` from typing `/hooks`.

- [ ] **Step 4: Run a real task**

Send the prompt: `请列出当前目录的文件`

Expected display sequence:
1. **thinking** — while Claude reasons
2. **working** — when it calls `Bash` or `LS`
3. **done** — when the response finishes
4. **idle** — 3 seconds after done

Daemon terminal should print one `-> {"state":"..."}` line per event.

- [ ] **Step 5: Trigger a Notification (waiting)**

Run a command that requires permission (e.g. ask Claude to make a destructive change in a sensitive folder). When the permission prompt shows up, display should switch to **waiting** with bouncing wide eyes and `?`.

- [ ] **Step 6: Trigger an error**

Ask Claude to: `运行命令 "exit 1"`. When the Bash tool returns non-zero, display should briefly show **error**.

- [ ] **Step 7: This is the final verification gate**

No commit (no code changed). If any step fails, file an issue / debug; if all pass, proceed to README.

---

## Task 18: README — full user docs

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Replace the skeleton with the full README**

```markdown
# clawd-mood

ESP32-C3 desk mascot that shows 7 pixel-eye expressions reflecting Claude Code state, over USB serial.

Built on [clawd-mochi](../projects/clawd-mochi/) hardware + [claudigotchi](../projects/claudigotchi/)-style plugin architecture.

## What it does

A Claude Code plugin hooks into session events and pipes the current state to an ESP32-C3 over USB serial. The ESP32 drives a 1.54" ST7789 TFT showing pixel-eye expressions:

| State           | Trigger                              | Look                                   |
| --------------- | ------------------------------------ | -------------------------------------- |
| Idle            | SessionStart, Stop after 3s, boot    | Normal eyes, slow wiggle + blink       |
| Thinking        | UserPromptSubmit                     | Eyes cycle up/left/right/center        |
| Working         | PreToolUse/PostToolUse/Subagent*     | Jitter + animated dots                 |
| Waiting         | Notification (Claude wants input)    | Wide eyes bouncing + `?`               |
| Done            | Stop (3s transient)                  | Squish smile `> <`                     |
| Error           | PostToolUseFailure                   | Asymmetric jittery eyes                |
| Sleeping        | 5 minutes of no events               | Closed-line eyes + floating `Z`        |

Any incoming state message wakes the device.

## Hardware

Identical to [clawd-mochi](../projects/clawd-mochi/):

| Part                | Spec                       |
| ------------------- | -------------------------- |
| ESP32-C3 Super Mini | with USB-C                 |
| ST7789 1.54" TFT    | 240×240 SPI                |
| Jumper wires ×8     | 8–10 cm                    |
| USB-C cable         | for data + power           |

### Wiring

| Display pin | ESP32-C3 GPIO  |
| ----------- | -------------- |
| VCC         | 3V3            |
| GND         | GND            |
| SDA         | GPIO 10 (MOSI) |
| SCL         | GPIO 8  (SCK)  |
| RES         | GPIO 2         |
| DC          | GPIO 1         |
| CS          | GPIO 4         |
| BL          | GPIO 3         |

⚠️ Connect VCC to **3.3V only** — never 5V.

## Setup

### Prerequisites

- macOS
- [uv](https://docs.astral.sh/uv/) — `brew install uv`
- jq — `brew install jq`
- Arduino IDE 2.x with the [ESP32 board package](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html)
- Arduino libraries (Library Manager): **Adafruit GFX Library**, **Adafruit ST7735 and ST7789 Library**, **ArduinoJson**

### 1. Flash the firmware

Open `firmware/clawd_mood/clawd_mood.ino` in Arduino IDE.

Tools → Board → **ESP32C3 Dev Module**. Then:

| Setting           | Value                  |
| ----------------- | ---------------------- |
| USB CDC On Boot   | **Enabled** ← required |
| CPU Frequency     | 160 MHz                |
| Upload Speed      | 921600                 |

Pick the right Port, then Upload.

### 2. Start the daemon

```bash
chmod +x plugin/scripts/daemon.py
./plugin/scripts/daemon.py
```

Keep it running. First run, uv will auto-install pyserial into a managed environment.

Override the serial port with `CLAWD_MOOD_PORT=/dev/cu.usbmodemXXX ./plugin/scripts/daemon.py` if needed.

### 3. Launch Claude Code with the plugin

```bash
claude --plugin-dir /absolute/path/to/clawd-mood/plugin
```

Optional permanent alias:
```bash
alias claude='claude --plugin-dir /absolute/path/to/clawd-mood/plugin'
```

### 4. Verify

Run `/hooks` inside Claude Code. You should see all 9 hook events registered to `clawd-mood`. The display should already be on `thinking`.

## Serial protocol

Newline-delimited JSON at 115200 baud, single direction (Mac → ESP32). Minimum message:

```json
{"state":"working"}
```

Optional debug fields:

```json
{"state":"working","event":"PreToolUse","tool":"Bash"}
```

Accepted states: `idle | thinking | working | waiting | done | error | sleeping`.

## Manual testing without Claude Code

With the daemon running:

```bash
echo '{"state":"working"}' > /tmp/clawd-mood.fifo
echo '{"state":"done"}'    > /tmp/clawd-mood.fifo
```

Or open Arduino Serial Monitor at 115200 baud with line ending = Newline and type JSON directly.

## Architecture

```
Claude Code → hook.sh → /tmp/clawd-mood.fifo → daemon.py → USB serial → ESP32-C3
```

See `docs/superpowers/specs/2026-06-07-clawd-mood-design.md` for the full design.

## License

MIT
```

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "docs: full user README"
```

---

## Self-review notes (for the implementing engineer)

This plan was written against the spec at `docs/superpowers/specs/2026-06-07-clawd-mood-design.md`. Quick coverage map:

| Spec § | Plan task |
|---|---|
| § 4 directory structure | Task 1, 2, 14, 15, 16 (each file creation) |
| § 5.1 7 states + triggers | Tasks 6–12 (one renderer per state) |
| § 5.2 Done→Idle, Sleep timer | Task 4 |
| § 5.3 multi-event collapse to working | Task 16 (hook.sh case) |
| § 6 NDJSON protocol | Task 3 (firmware) + Task 14 (daemon) |
| § 7.1 plugin.json | Task 15 |
| § 7.2 hooks.json | Task 15 |
| § 7.3 hook.sh | Task 16 |
| § 7.4 daemon.py PEP 723 + port detect | Task 14 |
| § 7.5 firmware skeleton | Task 2, 3, 4, 5 |
| § 8 error handling | Tasks 3, 4, 14, 16 each cover their own failure modes |
| § 9.1 firmware solo test | Task 13 |
| § 9.2 daemon test | Task 14 step 3 |
| § 9.3 e2e test | Task 17 |
| § 10 README | Task 18 |
