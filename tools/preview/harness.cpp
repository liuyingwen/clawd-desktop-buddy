// Runs the real firmware sketch on the host: real setup()/loop()/handleLine(), real Adafruit GFX,
// U8g2 and ArduinoJson, a virtual clock instead of time and a scene file instead of the serial port.
// Render time costs no virtual time, so identical input gives identical frames.
//
//   harness --scene FILE [--hashes FILE] [--frames FILE] [--report FILE] [--nvs key=value]... [--echo]
//   harness --themes     prints the compiled theme table as JSON
//
// A scene is NDJSON. Lines without "t" are metadata and ignored here:
//   {"t":ms,"send":{...}}   one line to the sketch's serial port, the object re-serialized as is
//   {"t":ms,"raw":"text"}   same, verbatim, for malformed input
//   {"t":ms,"record":bool}  frames are always rendered; this says whether they are logged (default on)
//   {"t":ms,"end":true}     stop; defaults to one second after the last line
#include <Arduino.h>
#include <SPI.h>
#include <Preferences.h>

#include <map>
#include <string>
#include <vector>

unsigned long g_host_ms = 0;
HostSerial Serial;
HostSPI SPI;
int g_host_rotation = -1;
int g_host_blk_pin = -1, g_host_blk_duty = -2, g_host_blk_bits = 0;
std::map<std::string, uint8_t> g_host_nvs;
int g_host_nvs_writes = 0;

static FILE* g_hashes = nullptr;
static FILE* g_frames = nullptr;
static bool g_record = true;
static long g_rendered = 0, g_recorded = 0, g_slides = 0;
static void frameHook(const uint16_t* buf, int w, int h);

void host_frame(const uint16_t* buf, int w, int h) { frameHook(buf, w, h); }

#include "clawd_mood.ino"

static void frameHook(const uint16_t* buf, int w, int h) {
  g_rendered++;
  if (!g_record) return;
  g_recorded++;
  // slideTo is the sketch's own global: the flag says the frame was drawn mid-slide
  bool sliding = slideTo >= 0;
  if (sliding) g_slides++;
  if (g_hashes) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    const uint8_t* p = (const uint8_t*)buf;
    for (size_t n = (size_t)w * h * 2; n--; p++) {
      hash ^= *p;
      hash *= 0x100000001b3ULL;
    }
    fprintf(g_hashes, "%lu %c %016llx\n", g_host_ms, sliding ? 'S' : '.', (unsigned long long)hash);
  }
  if (g_frames) fwrite(buf, 2, (size_t)w * h, g_frames);
}

struct Event {
  unsigned long t;
  std::string text;   // goes to serial when not empty
  int record;         // -1 = leave alone
};

static bool readScene(const char* path, std::vector<Event>& events, unsigned long& endMs) {
  FILE* f = fopen(path, "r");
  if (!f) return false;
  std::string body;
  char chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) body.append(chunk, n);
  fclose(f);
  bool ended = false;
  unsigned long last = 0;
  for (size_t pos = 0; pos < body.size();) {
    size_t nl = body.find('\n', pos);
    if (nl == std::string::npos) nl = body.size();
    std::string line = body.substr(pos, nl - pos);
    pos = nl + 1;
    if (line.empty()) continue;
    JsonDocument doc;
    if (deserializeJson(doc, line)) {
      fprintf(stderr, "bad scene line: %s\n", line.c_str());
      return false;
    }
    if (!doc["t"].is<unsigned long>()) continue;
    Event e{doc["t"].as<unsigned long>(), "", -1};
    last = std::max(last, e.t);
    if (doc["end"] | false) {
      endMs = e.t;
      ended = true;
      continue;
    }
    if (doc["send"].is<JsonObject>()) {
      char out[1024];
      serializeJson(doc["send"], out, sizeof(out));
      e.text = std::string(out) + "\n";
    } else if (doc["raw"].is<const char*>()) {
      e.text = std::string(doc["raw"].as<const char*>()) + "\n";
    } else if (doc["record"].is<bool>()) {
      e.record = doc["record"].as<bool>();
    }
    events.push_back(e);
  }
  if (!ended) endMs = last + 1000;
  return true;
}

static void runScene(const std::vector<Event>& events, unsigned long endMs) {
  setup();
  size_t next = 0;
  while (g_host_ms < endMs) {
    while (next < events.size() && events[next].t <= g_host_ms) {
      const Event& e = events[next++];
      if (e.record >= 0) g_record = e.record;
      Serial.rx.append(e.text);
    }
    loop();
  }
}

#if __has_include("theme.h")
#define HOST_HAS_THEMES 1
#endif

static void writeReport(const char* path) {
  FILE* f = fopen(path, "w");
  if (!f) return;
  fprintf(f, "{\"rendered\":%ld,\"recorded\":%ld,\"slide_frames\":%ld,\"sessions\":%d,", g_rendered, g_recorded,
          g_slides, sessionCount);
  fprintf(f, "\"serial_tx\":%zu,\"rotation\":%d,\"blk_pin\":%d,\"blk_duty\":%d,\"blk_bits\":%d,", Serial.txBytes,
          g_host_rotation, g_host_blk_pin, g_host_blk_duty, g_host_blk_bits);
#ifdef HOST_HAS_THEMES
  fprintf(f, "\"theme\":%d,", T->id);
#endif
  fprintf(f, "\"nvs_writes\":%d,\"nvs\":{", g_host_nvs_writes);
  bool first = true;
  for (const auto& kv : g_host_nvs) {
    fprintf(f, "%s\"%s\":%d", first ? "" : ",", kv.first.c_str(), kv.second);
    first = false;
  }
  fprintf(f, "}}\n");
  fclose(f);
}

#ifdef HOST_HAS_THEMES
static void dumpColors(const char* key, const uint16_t* c, size_t n) {
  printf("\"%s\":[", key);
  for (size_t i = 0; i < n; i++) printf("%s%u", i ? "," : "", c[i]);
  printf("],");
}

// The one list of themes: render.py takes names, taglines and colours from the compiled firmware
static void dumpThemes() {
  printf("[");
  bool first = true;
  for (const Theme* t : THEMES) {
    const Palette& p = t->pal;
    printf("%s{\"id\":%u,\"key\":\"%s\",\"name\":\"%s\",\"tagline\":\"%s\",\"glow\":\"#%06x\",\"min_fw\":%u,",
           first ? "" : ",", t->id, t->key, t->name, t->tagline, (unsigned)t->glow, t->minFw);
    first = false;
    printf("\"pal\":{");
#define ROLE(r) printf("\"" #r "\":%u,", p.r);
    ROLE(bg) ROLE(gap) ROLE(waitFlash) ROLE(errorFrame) ROLE(plate) ROLE(tileBorder) ROLE(text) ROLE(tag) ROLE(more)
    ROLE(pagerBg) ROLE(pagerOn) ROLE(pagerOff) ROLE(smoke) ROLE(sleepTarget) ROLE(toastBg) ROLE(toastText)
#undef ROLE
    dumpColors("fallback", p.fallback, 6);
    dumpColors("confetti", p.confetti, 6);
    printf("\"state\":[");
    for (int i = 0; i < 7; i++) printf("%s%u", i ? "," : "", p.state[i]);
    printf("]},\"skins\":[");
    for (int i = 0; i < SKIN_COUNT; i++) {
      const Skin& k = t->skins[i];
      printf("%s{\"src\":\"%s\",\"body\":%u,\"accent\":%u,\"face\":%u,\"eye\":%u,\"eye_on\":\"%c\",\"rows\":[", i ? "," : "",
             k.src, k.body, k.accent, k.face, k.eye, k.rows[k.eyeY][k.eyeL]);
      for (int r = 0; r < 7; r++) printf("%s\"%s\"", r ? "," : "", k.rows[r]);
      printf("]}");
    }
    printf("]}");
  }
  printf("]\n");
}
#endif

int main(int argc, char** argv) {
  const char* scene = nullptr;
  const char* report = nullptr;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--themes") {
#ifdef HOST_HAS_THEMES
      dumpThemes();
      return 0;
#else
      return 2;
#endif
    } else if (a == "--echo") {
      Serial.echo = true;
    } else if (i + 1 >= argc) {
      return 2;
    } else if (a == "--scene") {
      scene = argv[++i];
    } else if (a == "--hashes") {
      g_hashes = fopen(argv[++i], "w");
    } else if (a == "--frames") {
      g_frames = fopen(argv[++i], "wb");
    } else if (a == "--report") {
      report = argv[++i];
    } else if (a == "--nvs") {
      std::string kv = argv[++i];
      size_t eq = kv.find('=');
      if (eq == std::string::npos) return 2;
      g_host_nvs[kv.substr(0, eq)] = (uint8_t)atoi(kv.c_str() + eq + 1);
    } else {
      return 2;
    }
  }
  std::vector<Event> events;
  unsigned long endMs = 0;
  if (!scene || !readScene(scene, events, endMs)) return 2;
  runScene(events, endMs);
  if (g_hashes) fclose(g_hashes);
  if (g_frames) fclose(g_frames);
  if (report) writeReport(report);
  return 0;
}
