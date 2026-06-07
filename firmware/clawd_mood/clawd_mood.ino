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
  SPI.begin(8, -1, 10, TFT_CS);   // SCK=8, MOSI=10 (ESP32-C3 pin remap)
  tft.setSPISpeed(40000000);
  tft.init(DISP_W, DISP_H);
  tft.setRotation(2);
  lastEventMs = millis();
}

void loop() {
  pollSerial();
  renderMoodText();
  delay(30);
}
