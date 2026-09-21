#include "ui.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <M5Unified.h>

#include "spi_lock.h"

namespace ui {
namespace {

constexpr int kStripY = 240;  // touch only strip below the glass, y 240..279

constexpr uint16_t kBg = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kGrey = 0x7BEF;
constexpr uint16_t kDark = 0x2104;
constexpr uint16_t kBtn = 0x39E7;
constexpr uint16_t kGreen = 0x0640;
constexpr uint16_t kBrightGreen = 0x07E0;
constexpr uint16_t kYellow = 0xFFE0;
constexpr uint16_t kRed = 0xF800;
constexpr uint16_t kOrange = 0xFC00;
constexpr uint16_t kBlue = 0x022F;

enum Screen { S_MIXER, S_AMY, S_LOOPER, S_STUTTER, S_CONFIG, S_COUNT };

struct Rect {
  int x, y, w, h;
  bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// ---------------------------------------------------------------- state ----

// Written by the UI task, read by the audio task. Aligned 32 bit accesses are
// atomic on the ESP32, so plain volatiles are enough here.
volatile float gLevel[4] = {0.9f, 0.9f, 0.9f, 0.9f};
volatile bool gMute[4] = {false, false, false, false};
volatile float gPeak[4] = {0, 0, 0, 0};  // audio task raises, UI task reads and clears

int limiterIdx[4] = {0, 0, 0, 0};  // 0..6 means ceiling 0 to -6 dB
bool recEnable[3] = {false, false, false};
bool compOn = false;

struct Param {
  const char *name;
  int *value;
  int lo, hi;
  const char *const *names;  // non null means the value is an enum, tap cycles it
};

const char *const kRecStart[] = {"Level", "Now"};
const char *const kSig[] = {"4/4", "3/4"};
const char *const kOffOn[] = {"Off", "On"};
const char *const kAudioOut[] = {"USB", "Aux", "Both"};
const char *const kStutTarget[] = {"Looper", "Synth", "Main"};

int cfgRecStart = 0, cfgSig = 0, cfgAutoOd = 0, cfgTm = 0, cfgTmGap = 0, cfgAudioOut = 2;
int cfgSdRec = 0, cfgBpmMidi = 0, cfgLimRelease = 200, cfgStutTarget = 2;

const Param kConfig[] = {
    {"Rec start", &cfgRecStart, 0, 1, kRecStart},
    {"Time sig", &cfgSig, 0, 1, kSig},
    {"Auto overdub", &cfgAutoOd, 0, 1, kOffOn},
    {"Time machine", &cfgTm, 0, 1, kOffOn},
    {"TM gap beats", &cfgTmGap, 0, 4, nullptr},
    {"Audio out", &cfgAudioOut, 0, 2, kAudioOut},
    {"SD record", &cfgSdRec, 0, 1, kOffOn},
    {"BPM from MIDI", &cfgBpmMidi, 0, 1, kOffOn},
    {"Limiter rel ms", &cfgLimRelease, 0, 500, nullptr},
    {"Stutter target", &cfgStutTarget, 0, 2, kStutTarget},
};
constexpr int kConfigN = sizeof(kConfig) / sizeof(kConfig[0]);

struct AmyCh {
  int chan, patch, vol;
};
AmyCh amy[4] = {{1, 0, 100}, {2, 1, 100}, {3, 2, 100}, {10, 0, 100}};
int amyEdit = -1;
Param amyParams[3];

constexpr int kRowsPerPage = 5;
int cfgPage = 0;
int amyPage = 0;

enum LState { L_EMPTY, L_ARMED, L_REC, L_PLAY, L_STOP };
LState lstate = L_EMPTY;
bool overdub = false;
bool hasUndo = false;
int slot = 0;
int bpm = 120;
int measures = 4;
uint32_t recStartMs = 0;
uint32_t redUntil = 0;

const char *const kStutNames[12] = {"1/1", "1/2", "1/4",  "1/8",  "1/16", "1/32",
                                    "1/2T", "1/4T", "1/8T", "1/16T", "1/4D", "1/8D"};
int stutHeld = -1;

Screen screen = S_LOOPER;
bool dirty = true;

// Number editor
struct Editor {
  bool open = false;
  int *value = nullptr;
  int lo = 0, hi = 0;
  const char *title = "";
  bool tap = false;
  bool typing = false;
  int typed = 0;
} ed;

uint32_t tapTimes[4] = {0, 0, 0, 0};
int tapCount = 0;

// Meters
float dispLevel[4] = {0, 0, 0, 0}, holdLevel[4] = {0, 0, 0, 0};
uint32_t holdT[4] = {0, 0, 0, 0};
float recentMain = 0;
uint32_t lastTick = 0;

M5Canvas track(&M5.Display);

// Mixer geometry
constexpr int kTrackY = 22, kTrackH = 170, kTrackW = 24;
int colX(int c) { return c * 80; }
Rect trackHit(int c) { return {colX(c) + 2, kTrackY - 6, 32, kTrackH + 12}; }
Rect mixBtn(int c, int i) { return {colX(c) + 34, 22 + i * 58, 44, 50}; }

// ------------------------------------------------------------- drawing ----

void text(const char *s, int x, int y, float size, uint16_t fg, uint16_t bg, bool center = true) {
  auto &d = M5.Display;
  d.setTextSize(size);
  d.setTextColor(fg, bg);
  d.setTextDatum(center ? textdatum_t::middle_center : textdatum_t::middle_left);
  d.drawString(s, x, y);
}

void button(const Rect &r, const char *label, uint16_t fill, uint16_t fg = kWhite, float size = 1) {
  auto &d = M5.Display;
  d.fillRoundRect(r.x, r.y, r.w, r.h, 6, fill);
  const char *nl = strchr(label, '\n');
  if (!nl) {
    text(label, r.x + r.w / 2, r.y + r.h / 2, size, fg, fill);
    return;
  }
  char a[24];
  const size_t n = nl - label < 23 ? nl - label : 23;
  memcpy(a, label, n);
  a[n] = 0;
  const int off = (int)(size * 6) + 2;
  text(a, r.x + r.w / 2, r.y + r.h / 2 - off, size, fg, fill);
  text(nl + 1, r.x + r.w / 2, r.y + r.h / 2 + off, size, fg, fill);
}

void drawTrack(int c) {
  const int lv = (int)(dispLevel[c] * kTrackH);
  const int hv = (int)(holdLevel[c] * kTrackH);
  track.fillSprite(kDark);
  if (lv > 0) {
    const uint16_t col = dispLevel[c] > 0.95f ? kRed : (dispLevel[c] > 0.7f ? kYellow : kBrightGreen);
    track.fillRect(kTrackW - 8, kTrackH - lv, 8, lv, col);
  }
  if (hv > 1) track.fillRect(kTrackW - 8, kTrackH - hv, 8, 2, kWhite);
  const int fy = (int)((1.0f - gLevel[c]) * (kTrackH - 8));
  track.fillRect(0, fy, kTrackW - 9, 8, gMute[c] ? kGrey : kWhite);
  track.pushSprite(colX(c) + 4, kTrackY);
}

void drawMixer() {
  auto &d = M5.Display;
  d.fillScreen(kBg);
  static const char *names[4] = {"SYN", "AUX", "LOOP", "MAIN"};
  for (int c = 0; c < 4; c++) {
    drawTrack(c);
    button(mixBtn(c, 0), "M", gMute[c] ? kRed : kBtn, kWhite, 2);
    char lim[8];
    snprintf(lim, sizeof(lim), "%d", -limiterIdx[c]);
    button(mixBtn(c, 1), lim, limiterIdx[c] ? kBlue : kBtn, kWhite, 2);
    if (c < 3) {
      const Rect r = mixBtn(c, 2);
      button(r, "", recEnable[c] ? kOrange : kBtn, kWhite, 2);
      const int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
      d.fillArc(cx, cy, 9, 13, 300, 240, kWhite);  // open ring, the loop arrow body
      d.fillTriangle(cx + 4, cy - 17, cx + 14, cy - 10, cx + 2, cy - 6, kWhite);
    } else {
      button(mixBtn(c, 2), "PUMP", compOn ? kOrange : kBtn, kWhite, 1);
    }
    text(names[c], colX(c) + 40, 208, 2, kWhite, kBg);
    d.drawFastVLine(colX(c) + 79, 0, kStripY, kDark);
  }
}

void drawAmySummary() {
  auto &d = M5.Display;
  d.fillScreen(kBg);
  for (int i = 0; i < 4; i++) {
    const Rect r = {(i % 2) * 160 + 3, (i / 2) * 120 + 3, 154, 114};
    d.fillRoundRect(r.x, r.y, r.w, r.h, 8, kDark);
    char b[24];
    snprintf(b, sizeof(b), "CH %d", amy[i].chan);
    text(b, r.x + 12, r.y + 24, 3, kWhite, kDark, false);
    snprintf(b, sizeof(b), amy[i].chan == 10 ? "Drums %d" : "Patch %d", amy[i].patch);
    text(b, r.x + 12, r.y + 62, 2, kGrey, kDark, false);
    snprintf(b, sizeof(b), "Vol %d", amy[i].vol);
    text(b, r.x + 12, r.y + 92, 2, kGrey, kDark, false);
  }
}

// Parameter list with a page scroll bar on the right. showX draws the back X.
void paramValue(const Param &p, char *out, size_t n) {
  if (p.names)
    snprintf(out, n, "%s", p.names[*p.value - p.lo]);
  else
    snprintf(out, n, "%d", *p.value);
}

void drawList(const Param *p, int n, const char *title, int page, bool showX) {
  auto &d = M5.Display;
  d.fillScreen(kBg);
  if (showX) button({0, 0, 44, 30}, "X", kRed, kWhite, 2);
  text(title, showX ? 70 : 8, 15, 2, kWhite, kBg, false);
  const int pages = (n + kRowsPerPage - 1) / kRowsPerPage;
  for (int i = 0; i < kRowsPerPage; i++) {
    const int idx = page * kRowsPerPage + i;
    if (idx >= n) break;
    const Rect r = {4, 34 + i * 41, 278, 38};
    d.fillRoundRect(r.x, r.y, r.w, r.h, 6, kDark);
    text(p[idx].name, r.x + 8, r.y + 19, 2, kWhite, kDark, false);
    char v[16];
    paramValue(p[idx], v, sizeof(v));
    button({r.x + 170, r.y + 4, 100, 30}, v, kBtn, kYellow, 2);
  }
  button({288, 34, 30, 32}, "^", kBtn, kWhite, 2);
  button({288, 202, 30, 32}, "v", kBtn, kWhite, 2);
  const int segH = 130 / pages;
  for (int s = 0; s < pages; s++)
    d.fillRect(288, 68 + s * segH + 1, 30, segH - 2, s == page ? kBrightGreen : kBtn);
}

void drawLooper() {
  auto &d = M5.Display;
  d.fillScreen(kBg);
  for (int i = 0; i < 4; i++) {
    char b[4];
    snprintf(b, sizeof(b), "%d", i + 1);
    button({i * 80 + 3, 3, 74, 114}, b, i == slot ? kGreen : kBtn, kWhite, 4);
  }
  char b[16];
  snprintf(b, sizeof(b), "BPM\n%d", bpm);
  button({3, 123, 74, 114}, b, kBtn, kWhite, 2);
  snprintf(b, sizeof(b), "MEAS\n%d", measures);
  button({83, 123, 74, 114}, b, kBtn, kWhite, 2);

  const char *stop = "STOP";
  if (lstate == L_STOP) stop = "CLEAR";
  if (lstate == L_EMPTY) stop = hasUndo ? "UNDO" : "-";
  button({163, 123, 74, 114}, stop, lstate == L_EMPTY && !hasUndo ? kDark : kBtn, kWhite, 2);

  const char *play = "PLAY";
  uint16_t fill = kBtn;
  switch (lstate) {
    case L_PLAY:
      play = "OVERDUB";
      fill = overdub ? kRed : kBtn;
      break;
    case L_STOP:
      play = "PLAY";
      fill = kGreen;
      break;
    case L_EMPTY:
      play = cfgTm ? "CAPTURE" : "ARM";
      fill = millis() < redUntil ? kRed : kBtn;
      break;
    case L_ARMED:
      play = "REC NOW";
      fill = kOrange;
      break;
    case L_REC:
      play = "REC..";
      fill = kRed;
      break;
  }
  button({243, 123, 74, 114}, play, fill, kWhite, lstate == L_PLAY ? 1 : 2);
}

void drawStutter() {
  auto &d = M5.Display;
  d.fillScreen(kBg);
  for (int i = 0; i < 12; i++) {
    button({(i % 4) * 80 + 3, (i / 4) * 80 + 3, 74, 74}, kStutNames[i], i == stutHeld ? kOrange : kBtn,
           kWhite, 2);
  }
}

void drawEditor() {
  auto &d = M5.Display;
  d.fillScreen(kBg);
  button({0, 0, 44, 30}, "X", kRed, kWhite, 2);
  text(ed.title, 70, 15, 2, kWhite, kBg, false);
  char b[16];
  snprintf(b, sizeof(b), "%d", *ed.value);
  text(b, 100, 58, 5, kYellow, kBg);
  static const char *pad[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "C", "0", "OK"};
  for (int i = 0; i < 12; i++)
    button({(i % 3) * 66 + 2, 88 + (i / 3) * 38, 62, 35}, pad[i], i == 11 ? kGreen : kBtn, kWhite, 2);
  button({208, 34, 108, 52}, "UP", kBtn, kWhite, 2);
  button({208, 92, 108, 52}, "DOWN", kBtn, kWhite, 2);
  if (ed.tap) button({208, 150, 108, 52}, "TAP", kOrange, kWhite, 2);
}

void redraw() {
  spi_lock::Guard lock;
  if (ed.open) return drawEditor();
  switch (screen) {
    case S_MIXER: return drawMixer();
    case S_AMY:
      if (amyEdit >= 0) {
        char t[16];
        snprintf(t, sizeof(t), "CH %d", amy[amyEdit].chan);
        return drawList(amyParams, 3, t, 0, true);
      }
      return drawAmySummary();
    case S_LOOPER: return drawLooper();
    case S_STUTTER: return drawStutter();
    case S_CONFIG: return drawList(kConfig, kConfigN, "CONFIG", cfgPage, false);
    default: break;
  }
}

// -------------------------------------------------------------- logic ----

void openEditor(const char *title, int *v, int lo, int hi, bool tap) {
  ed.open = true;
  ed.value = v;
  ed.lo = lo;
  ed.hi = hi;
  ed.title = title;
  ed.tap = tap;
  ed.typing = false;
  ed.typed = 0;
  dirty = true;
}

void setValue(int v) {
  if (v < ed.lo) v = ed.lo;
  if (v > ed.hi) v = ed.hi;
  *ed.value = v;
}

void tapTempo(uint32_t now) {
  if (tapCount > 0 && now - tapTimes[(tapCount - 1) % 4] > 2000) tapCount = 0;
  tapTimes[tapCount % 4] = now;
  tapCount++;
  if (tapCount >= 2) {
    const int n = tapCount < 4 ? tapCount : 4;
    const uint32_t newest = tapTimes[(tapCount - 1) % 4];
    const uint32_t oldest = tapTimes[(tapCount - n) % 4];
    const uint32_t avg = (newest - oldest) / (n - 1);
    if (avg > 0) setValue((int)(60000 / avg));
  }
}

void pressEditor(int x, int y) {
  if (Rect{0, 0, 44, 30}.hit(x, y)) {
    ed.open = false;
    dirty = true;
    return;
  }
  for (int i = 0; i < 12; i++) {
    if (!Rect{(i % 3) * 66 + 2, 88 + (i / 3) * 38, 62, 35}.hit(x, y)) continue;
    if (i == 11) {
      ed.open = false;
    } else if (i == 9) {
      ed.typing = false;
      ed.typed = 0;
      setValue(ed.lo);
    } else {
      const int digit = i == 10 ? 0 : i + 1;
      ed.typed = ed.typing ? ed.typed * 10 + digit : digit;
      if (ed.typed > ed.hi) ed.typed = digit;
      ed.typing = true;
      setValue(ed.typed);
    }
    dirty = true;
    return;
  }
  if (Rect{208, 34, 108, 52}.hit(x, y)) setValue(*ed.value + 1), ed.typing = false, dirty = true;
  if (Rect{208, 92, 108, 52}.hit(x, y)) setValue(*ed.value - 1), ed.typing = false, dirty = true;
  if (ed.tap && Rect{208, 150, 108, 52}.hit(x, y)) tapTempo(millis()), dirty = true;
}

void pressList(const Param *p, int n, int &page, int x, int y, bool isAmy) {
  const int pages = (n + kRowsPerPage - 1) / kRowsPerPage;
  if (isAmy && Rect{0, 0, 44, 30}.hit(x, y)) {
    amyEdit = -1;
    dirty = true;
    return;
  }
  if (Rect{288, 34, 30, 32}.hit(x, y) && page > 0) page--, dirty = true;
  if (Rect{288, 202, 30, 32}.hit(x, y) && page < pages - 1) page++, dirty = true;
  if (x >= 288 && y >= 68 && y < 198) {
    const int s = (y - 68) / (130 / pages);
    if (s < pages) page = s, dirty = true;
  }
  for (int i = 0; i < kRowsPerPage; i++) {
    const int idx = page * kRowsPerPage + i;
    if (idx >= n) break;
    if (!Rect{4, 34 + i * 41, 278, 38}.hit(x, y)) continue;
    const Param &q = p[idx];
    if (q.names) {
      *q.value = *q.value >= q.hi ? q.lo : *q.value + 1;
      dirty = true;
    } else {
      openEditor(q.name, q.value, q.lo, q.hi, false);
    }
    return;
  }
}

void pressMixer(int x, int y) {
  for (int c = 0; c < 4; c++) {
    if (mixBtn(c, 0).hit(x, y)) gMute[c] = !gMute[c], dirty = true;
    if (mixBtn(c, 1).hit(x, y)) limiterIdx[c] = (limiterIdx[c] + 1) % 7, dirty = true;
    if (mixBtn(c, 2).hit(x, y)) {
      if (c < 3)
        recEnable[c] = !recEnable[c];
      else
        compOn = !compOn;
      dirty = true;
    }
  }
}

int dragCol = -1;

void pressLooper(int x, int y) {
  for (int i = 0; i < 4; i++)
    if (Rect{i * 80 + 3, 3, 74, 114}.hit(x, y)) slot = i, dirty = true;
  if (Rect{3, 123, 74, 114}.hit(x, y)) openEditor("BPM", &bpm, 20, 300, true);
  if (Rect{83, 123, 74, 114}.hit(x, y)) openEditor("Measures", &measures, 1, 8, false);
  if (Rect{163, 123, 74, 114}.hit(x, y)) {
    switch (lstate) {
      case L_PLAY:
      case L_REC: lstate = L_STOP; overdub = false; break;
      case L_ARMED: lstate = L_EMPTY; break;
      case L_STOP: lstate = L_EMPTY; hasUndo = true; break;
      case L_EMPTY:
        if (hasUndo) lstate = L_STOP, hasUndo = false;
        break;
    }
    dirty = true;
  }
  if (Rect{243, 123, 74, 114}.hit(x, y)) {
    switch (lstate) {
      case L_PLAY: overdub = !overdub; break;
      case L_STOP: lstate = L_PLAY; break;
      case L_EMPTY:
        if (cfgTm) {
          // Capture needs audio already heard, otherwise flash red.
          if (recentMain > 0.02f)
            lstate = L_PLAY;
          else
            redUntil = millis() + 600;
        } else {
          lstate = L_ARMED;
        }
        break;
      case L_ARMED: lstate = L_REC; recStartMs = millis(); break;
      case L_REC: lstate = L_PLAY; break;
    }
    dirty = true;
  }
}

void pressAmy(int x, int y) {
  if (amyEdit >= 0) return pressList(amyParams, 3, amyPage, x, y, true);
  for (int i = 0; i < 4; i++) {
    if (!Rect{(i % 2) * 160 + 3, (i / 2) * 120 + 3, 154, 114}.hit(x, y)) continue;
    amyEdit = i;
    amyPage = 0;
    amyParams[0] = {"MIDI chan", &amy[i].chan, 1, 16, nullptr};
    amyParams[1] = {"Patch", &amy[i].patch, 0, 255, nullptr};
    amyParams[2] = {"Volume", &amy[i].vol, 0, 100, nullptr};
    dirty = true;
  }
}

void onPress(int x, int y) {
  if (ed.open) return pressEditor(x, y);
  switch (screen) {
    case S_MIXER:
      pressMixer(x, y);
      dragCol = -1;
      for (int c = 0; c < 4; c++)
        if (trackHit(c).hit(x, y)) dragCol = c;
      break;
    case S_AMY: pressAmy(x, y); break;
    case S_LOOPER: pressLooper(x, y); break;
    case S_CONFIG: pressList(kConfig, kConfigN, cfgPage, x, y, false); break;
    default: break;
  }
}

void whileHeld(int x, int y) {
  if (ed.open) return;
  if (screen == S_MIXER && dragCol >= 0) {
    float v = 1.0f - (float)(y - kTrackY - 4) / (kTrackH - 8);
    gLevel[dragCol] = v < 0 ? 0 : (v > 1 ? 1 : v);
  }
  if (screen == S_STUTTER && y < kStripY) {
    const int i = (y / 80) * 4 + (x / 80);
    if (i != stutHeld && i >= 0 && i < 12) stutHeld = i, dirty = true;
  }
}

void gotoScreen(int delta) {
  screen = (Screen)((screen + delta + S_COUNT) % S_COUNT);
  stutHeld = -1;
  dirty = true;
}

void tick(uint32_t now) {
  if (now - lastTick < 40) return;
  lastTick = now;
  for (int c = 0; c < 4; c++) {
    const float p = gPeak[c];
    gPeak[c] = 0;
    dispLevel[c] = p > dispLevel[c] ? p : dispLevel[c] * 0.82f;
    if (p >= holdLevel[c]) {
      holdLevel[c] = p;
      holdT[c] = now;
    } else if (now - holdT[c] > 700) {
      holdLevel[c] *= 0.9f;
    }
  }
  recentMain = dispLevel[3] > recentMain ? dispLevel[3] : recentMain * 0.9f;

  const int beats = cfgSig == 0 ? 4 : 3;
  if (lstate == L_ARMED && !cfgTm && cfgRecStart == 0 && dispLevel[3] > 0.05f) {
    lstate = L_REC;
    recStartMs = now;
    dirty = true;
  }
  if (lstate == L_REC && now - recStartMs >= (uint32_t)(measures * beats * 60000L / bpm)) {
    lstate = L_PLAY;
    dirty = true;
  }
  if (redUntil && now >= redUntil) {
    redUntil = 0;
    dirty = true;
  }

  if (!dirty && screen == S_MIXER && !ed.open) {
    spi_lock::Guard lock;
    for (int c = 0; c < 4; c++) drawTrack(c);
  }
}

}  // namespace

// ---------------------------------------------------------------- API ----

void begin() {
  track.createSprite(kTrackW, kTrackH);
  dirty = true;
}

void update() {
  M5.update();
  const uint32_t now = millis();
  const auto t = M5.Touch.getDetail();
  if (t.wasPressed() && t.y < kStripY) onPress(t.x, t.y);
  if (t.isPressed() && t.y < kStripY) whileHeld(t.x, t.y);
  if (t.wasReleased()) {
    if (t.base_y >= kStripY && !ed.open && amyEdit < 0) {
      const int dx = t.x - t.base_x;
      if (dx > 40)
        gotoScreen(-1);
      else if (dx < -40)
        gotoScreen(1);
      else if (abs(dx) <= 40)
        gotoScreen(t.base_x < 160 ? -1 : 1);
    }
    if (stutHeld >= 0) stutHeld = -1, dirty = true;
    dragCol = -1;
  }
  tick(now);
  if (dirty) {
    dirty = false;
    redraw();
  }
}

void processBlock(int16_t *b, int frames) {
  const float g0 = gMute[0] ? 0.0f : gLevel[0] * gLevel[0];
  const float g3 = gMute[3] ? 0.0f : gLevel[3] * gLevel[3];
  int p0 = 0, p3 = 0;
  for (int i = 0; i < frames * 2; i++) {
    const float s0 = b[i] * g0;
    float s3 = s0 * g3;
    const int a0 = (int)fabsf(s0);
    const int a3 = (int)fabsf(s3);
    if (a0 > p0) p0 = a0;
    if (a3 > p3) p3 = a3;
    if (s3 > 32767.0f) s3 = 32767.0f;
    if (s3 < -32768.0f) s3 = -32768.0f;
    b[i] = (int16_t)s3;
  }
  const float f0 = p0 / 32768.0f, f3 = p3 / 32768.0f;
  if (f0 > gPeak[0]) gPeak[0] = f0;
  if (f3 > gPeak[3]) gPeak[3] = f3;
}

int stutterDivision() { return stutHeld; }

}  // namespace ui
