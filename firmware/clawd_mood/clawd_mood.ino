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

#define DONE_REVERT_MS    3000UL          // Done → Idle after 3s
#define SLEEP_IDLE_MS     (5UL*60*1000)   // 5 minutes

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

// blink: 0 = open, 1 = half-closed, 2 = closed
void drawNormalEyes(int16_t ox = 0, uint8_t blink = 0) {
  tft.fillScreen(bgColor);
  const int16_t lx = eyeLX(ox), rx = eyeRX(ox), ey = eyeY();
  int16_t h;
  switch (blink) {
    case 0:  h = EYE_H;     break;
    case 1:  h = EYE_H / 2; break;
    default: h = 6;         break;
  }
  int16_t y = ey + (EYE_H - h) / 2;
  tft.fillRect(lx, y, EYE_W, h, C_BLACK);
  tft.fillRect(rx, y, EYE_W, h, C_BLACK);
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

void tickMoodMachine() {
  unsigned long now = millis();
  if (currentMood == MOOD_DONE && now - doneEnteredMs > DONE_REVERT_MS) {
    setMood(MOOD_IDLE);
  }
  if (currentMood != MOOD_SLEEPING && now - lastEventMs > SLEEP_IDLE_MS) {
    setMood(MOOD_SLEEPING);
  }
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

// ── Per-mood renderers ──────────────────────────────────────────
// Each renderer is called every frame (~33fps). They use a static
// frame counter for animation. They redraw only when needed.

void drawIdle() {
  static uint8_t step = 0;
  static unsigned long lastWiggle = 0;
  static unsigned long blinkStart = 0;   // 0 = not blinking
  static unsigned long nextBlinkAt = 0;
  static uint8_t lastPhase = 0;
  unsigned long now = millis();

  if (nextBlinkAt == 0) nextBlinkAt = now + 5000;

  // Blink animation: half-closed → closed → half-closed → open, ~280ms total
  uint8_t phase = 0;
  if (blinkStart) {
    unsigned long el = now - blinkStart;
    if      (el < 80)  phase = 1;
    else if (el < 200) phase = 2;
    else if (el < 280) phase = 1;
    else { blinkStart = 0; phase = 0; }
  }

  if (!blinkStart && now >= nextBlinkAt) {
    blinkStart = now;
    nextBlinkAt = now + 5000 + (now % 2500);  // 5–7.5s
    phase = 1;
  }

  bool blinking = phase != 0;
  bool wiggleDue = now - lastWiggle > 800;
  bool phaseEdge = phase != lastPhase;
  if (moodDirty || phaseEdge || (wiggleDue && !blinking)) {
    const int16_t offs[] = {0, -4, 0, 4, 0};
    drawNormalEyes(offs[step % 5], phase);
    if (wiggleDue && !blinking) {
      step++;
      lastWiggle = now;
    }
    lastPhase = phase;
    moodDirty = false;
  }
}

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

void renderMood() {
  switch (currentMood) {
    case MOOD_IDLE:     drawIdle(); break;
    case MOOD_THINKING: drawThinking(); break;
    case MOOD_WORKING:  drawWorking(); break;
    case MOOD_WAITING:  drawWaiting(); break;
    case MOOD_DONE: drawDone(); break;
    case MOOD_ERROR: drawError(); break;
    case MOOD_SLEEPING: drawSleeping(); break;
    default: break;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(TFT_BLK, OUTPUT); digitalWrite(TFT_BLK, HIGH);
  SPI.begin(8, -1, 10, TFT_CS);   // SCK=8, MOSI=10 (ESP32-C3 pin remap)
  tft.setSPISpeed(40000000);
  tft.init(DISP_W, DISP_H);
  tft.setRotation(1);
  initColors();
  lastEventMs = millis();
}

void loop() {
  pollSerial();
  tickMoodMachine();
  renderMood();
  delay(30);
}
