#include "ui.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <M5Unified.h>
#include <Preferences.h>

#include "config.h"
#include "looper.h"
#include "patch_names.h"
#include "spi_lock.h"
#include "stutter.h"
#include "synth_engine.h"

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
// atomic on the ESP32, so plain volatiles are enough here. Channels are
// 0 INT (synth), 1 EXT (aux in, not wired yet), 2 LOP (looper return), 3 ALL.
// INT, EXT, LOP, ALL. EXT defaults to 75%, not full: it's a live mic/line
// input, and a hot analog input on this board can bleed into what should be
// a quiet aux channel, see FIRMWARE_PLAN.md's Mixer status notes.
volatile float gLevel[4] = {1.0f, 0.75f, 1.0f, 1.0f};
volatile bool gMute[4] = {false, false, false, false};
volatile bool gSolo[3] = {false, false, false};
volatile bool gRecEnable[2] = {true, false};  // INT and EXT feed the looper
volatile float gPeak[4] = {0, 0, 0, 0};       // audio task raises, UI task reads and clears

int limiterIdx[4] = {0, 0, 0, 0};  // 0..6 means Threshold 0 to -6 dB (SP1LimiterJS's slider1)
bool compOn = false;               // pump compressor (not applied yet)

// SP1LimiterJS "Simple Peak-1 Limiter" (Michael Gruhn 2006, LOSER pack,
// fetched from Samelot/Reaper's Effects/LOSER folder, ported line for line
// rather than from the MGA_JSLimiter this project tried first, see
// FIRMWARE_PLAN.md's Mixer status notes for why). One knob (Threshold, here
// driven by limiterIdx same as before): gain = max(rms, thresh), output =
// input / gain. Below threshold that DIVIDES by thresh < 1, i.e. automatic
// makeup gain toward 0dBFS, above threshold it divides by the envelope
// itself, which self-bounds the output to +-1.0 by construction (whichever
// channel is loudest lands exactly at the ceiling, no separate clamp needed
// mathematically, kept anyway as float/int16 boundary insurance). This is
// the "auto turns up to compensate" behavior MGA never had, MGA only ever
// reduces gain, never adds makeup gain.
struct LimiterState {
  float t = 0.0f;  // one pole lowpass state (10Hz corner), Gruhn's envelope smoother
};
LimiterState limSt[4];

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
const char *const kStutTrack[] = {"Looper", "Synth", "Main", "Aux"};

int cfgRecStart = 0, cfgSig = 0, cfgAutoOd = 0, cfgTm = 0, cfgTmGap = 0, cfgAudioOut = 2;
int cfgSdRec = 0, cfgBpmMidi = 0, cfgStutTrack = 2;
// Int/Ext Max Gain Db: brought back per direct hardware feedback that it was
// actually helping (removing it was this project's own reasoning that SP1's
// makeup gain made it redundant, that reasoning was wrong, or at least not
// what the ear preferred), default dropped from 12 to 6. Stacks with SP1's
// own automatic makeup gain rather than replacing it.
int cfgIntMaxGainDb = 6, cfgExtMaxGainDb = 0;
// Lim Rel Ms stays gone: SP1LimiterJS (below) hardcodes its envelope's
// release behavior (a fixed 10Hz one pole, not a separate attack/release
// pair), matching its "Simple" name, no release control to wire this to
// any more. MGA_JSLimiter, tried first, did have one, this project's
// second, closer look at the LOSER pack chose SP1 over MGA specifically
// for its automatic makeup gain, this setting is the tradeoff.

const Param kConfig[] = {
    {"Rec Start", &cfgRecStart, 0, 1, kRecStart},
    {"Time Sig", &cfgSig, 0, 1, kSig},
    {"Auto Overdub", &cfgAutoOd, 0, 1, kOffOn},
    {"Time Machine", &cfgTm, 0, 1, kOffOn},
    {"TM Gap Beats", &cfgTmGap, 0, 4, nullptr},
    {"Audio Out", &cfgAudioOut, 0, 2, kAudioOut},
    {"SD Record", &cfgSdRec, 0, 1, kOffOn},
    {"BPM From MIDI", &cfgBpmMidi, 0, 1, kOffOn},
    {"Stutter Track", &cfgStutTrack, 0, 3, kStutTrack},
    {"Int Max Gain", &cfgIntMaxGainDb, 0, 24, nullptr},
    {"Ext Max Gain", &cfgExtMaxGainDb, 0, 24, nullptr},
};
constexpr int kConfigN = sizeof(kConfig) / sizeof(kConfig[0]);

struct AmyCh {
  int chan, patch, vol, voices;
};
// voices 6 matches AMY's own polyphony for a fresh default_synths channel,
// so this changes nothing until a user actually edits it. Each voice of a
// Juno style patch is ~5 oscillators (see synth.md), and all 4 channels
// share one 250 oscillator pool (config.h's LOOPANINI_DRUM_OSC_BASE/COUNT
// also carve 8 out of it for sample drums), so cranking every channel to
// the top of the UI range at once can ask for more than the pool holds.
// AMY's own voice stealing handles that gracefully, it is not a hard error.
AmyCh amy[4] = {{1, 0, 100, 6}, {2, 1, 100, 6}, {3, 2, 100, 6}, {10, 0, 100, 6}};
AmyCh amySeen[4] = {{1, 0, 100, 6}, {2, 1, 100, 6}, {3, 2, 100, 6}, {10, 0, 100, 6}};
int amyChanSeen[4] = {1, 2, 3, 10};
int amySynthId[4] = {1, 2, 3, 10};  // current AMY synth number per UI slot, moves with to_synth
int amyEdit = -1;
constexpr int kAmyParamsN = 4;
Param amyParams[kAmyParamsN];

constexpr int kRowsPerPage = 5;
int cfgPage = 0;
int amyPage = 0;
bool patchPicker = false;  // true while the named patch list (not amyParams) is open
int patchPickerPage = 0;
bool chanPicker = false;  // true while the 16 button MIDI channel grid is open

int slot = 0;
int bpm = 120;
int measures = 4;
uint32_t redUntil = 0;
uint32_t seenRejects = 0;

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
uint32_t lastTick = 0;
uint32_t lastMeterDraw = 0;
// What each column's sprite was actually last pushed for, so a column
// that hasn't visibly moved (level, fader drag, or mute) doesn't cost an
// SPI transaction it doesn't need, see tick()'s comment on why this
// matters more than it looks like it should.
float meterSeen[4] = {-1, -1, -1, -1};
float faderSeen[4] = {-1, -1, -1, -1};
bool muteSeen[4] = {false, false, false, false};
looper::State seenState = looper::EMPTY;
bool seenOd = false, seenUndo = false;

M5Canvas track(&M5.Display);

// Mixer geometry: a full height track on the left of each 80 px column, four
// round buttons stacked to its right, the channel title under the buttons.
// kTrackW is 1px wider than the visual slider so the handle circle (radius
// kTrackW/2, centered at kTrackW/2) has room on its right edge without
// clipping in the sprite; the extra column is on the right, the slider's
// left edge and screen position don't move.
constexpr int kTrackW = 25, kTrackH = 240;
int colX(int c) { return c * 80; }
Rect trackHit(int c) { return {colX(c) + 2, 0, 32, kTrackH}; }
int btnCy(int i) { return 26 + i * 54; }
Rect mixBtn(int c, int i) { return {colX(c) + 38, btnCy(i) - 20, 40, 40}; }

// ------------------------------------------------------------- drawing ----

void text(const char *s, int x, int y, float size, uint16_t fg, uint16_t bg, bool center = true) {
  auto &d = M5.Display;
  d.setTextSize(size);
  d.setTextColor(fg, bg);
  d.setTextDatum(center ? textdatum_t::middle_center : textdatum_t::middle_left);
  d.drawString(s, x, y);
}

// Cuts s off (no ellipsis) at the last character that still fits maxW
// pixels at the given text size, measured for real via textWidth() rather
// than guessed by character count: proportional fonts and the variety of
// patch name lengths make a fixed character count wrong in both
// directions depending on which letters are actually in the name.
void truncateToWidth(const char *s, float size, int maxW, char *out, size_t outSize) {
  auto &d = M5.Display;
  d.setTextSize(size);
  snprintf(out, outSize, "%s", s);
  while (out[0] && d.textWidth(out) > maxW) out[strlen(out) - 1] = '\0';
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

void circleButton(const Rect &r, const char *label, uint16_t fill, uint16_t fg = kWhite, float size = 2) {
  M5.Display.fillCircle(r.x + r.w / 2, r.y + r.h / 2, r.w / 2, fill);
  text(label, r.x + r.w / 2 + 1, r.y + r.h / 2 + 1, size, fg, fill);
}

// Outline only, background colored inside so it reads as hollow against the
// mixer's black background. Used for the stutter-target toggle, the one
// mixer button that isn't a solid fill like the others.
void circleButtonHollow(const Rect &r, const char *label, uint16_t ring, uint16_t fg = kWhite,
                         float size = 2) {
  auto &d = M5.Display;
  const int cx = r.x + r.w / 2, cy = r.y + r.h / 2, rad = r.w / 2;
  d.fillCircle(cx, cy, rad, kBg);
  d.drawCircle(cx, cy, rad, ring);
  d.drawCircle(cx, cy, rad - 1, ring);
  text(label, cx + 1, cy + 1, size, fg, kBg);
}

void triangle(int cx, int cy, int half, bool up, uint16_t color) {
  auto &d = M5.Display;
  if (up)
    d.fillTriangle(cx, cy - half, cx - half, cy + half, cx + half, cy + half, color);
  else
    d.fillTriangle(cx - half, cy - half, cx + half, cy - half, cx, cy + half, color);
}

enum Icon { I_REC, I_ARM, I_PLAY, I_PAUSE, I_STOP, I_CLEAR, I_UNDO, I_LOOP };

void icon(Icon k, int cx, int cy, int s, uint16_t fg, uint16_t bg) {
  auto &d = M5.Display;
  switch (k) {
    case I_REC:
      d.fillCircle(cx, cy, s, fg);
      break;
    case I_ARM:
      d.fillCircle(cx, cy, s, fg);
      d.fillCircle(cx, cy, s - 4, bg);
      break;
    case I_PLAY:
      d.fillTriangle(cx - s * 2 / 3, cy - s, cx - s * 2 / 3, cy + s, cx + s, cy, fg);
      break;
    case I_PAUSE:
      d.fillRect(cx - s + 2, cy - s, s * 2 / 3 + 2, s * 2, fg);
      d.fillRect(cx + s / 3 - 1, cy - s, s * 2 / 3 + 2, s * 2, fg);
      break;
    case I_STOP:
      d.fillRect(cx - s + 2, cy - s + 2, s * 2 - 4, s * 2 - 4, fg);
      break;
    case I_CLEAR:
      d.fillCircle(cx, cy, s, fg);
      d.fillCircle(cx, cy, s - 3, bg);
      for (int o = -1; o <= 1; o++) {
        d.drawLine(cx - s / 2 + o, cy - s / 2, cx + s / 2 + o, cy + s / 2, fg);
        d.drawLine(cx - s / 2 + o, cy + s / 2, cx + s / 2 + o, cy - s / 2, fg);
      }
      break;
    case I_UNDO:
      d.fillArc(cx, cy, s - 4, s, 270, 210, fg);
      d.fillTriangle(cx - 9, cy - s + 2, cx + 3, cy - s - 6, cx + 3, cy - s + 10, fg);
      break;
    case I_LOOP:
      d.fillArc(cx, cy, 9, 13, 300, 240, fg);
      d.fillTriangle(cx + 4, cy - 17, cx + 14, cy - 10, cx + 2, cy - 6, fg);
      break;
  }
}

void drawTrack(int c) {
  const int lv = (int)(dispLevel[c] * kTrackH);
  const int hv = (int)(holdLevel[c] * kTrackH);
  track.fillSprite(kDark);
  if (lv > 0) {
    const uint16_t col = dispLevel[c] > 0.95f ? kRed : (dispLevel[c] > 0.7f ? kYellow : kBrightGreen);
    track.fillRect(kTrackW / 2 - 4, kTrackH - lv, 8, lv, col);
  }
  if (hv > 1) track.fillRect(kTrackW / 2 - 4, kTrackH - hv, 8, 2, kWhite);
  const int cy = kTrackW / 2 + (int)((1.0f - gLevel[c]) * (kTrackH - kTrackW));
  track.fillCircle(kTrackW / 2, cy, kTrackW / 2, gMute[c] ? kGrey : kWhite);
  track.pushSprite(colX(c) + 6, 0);
}

void drawMixer() {
  auto &d = M5.Display;
  d.fillScreen(kBg);
  static const char *names[4] = {"INT", "EXT", "LOP", "ALL"};
  const bool anySolo = gSolo[0] || gSolo[1] || gSolo[2];
  for (int c = 0; c < 4; c++) {
    drawTrack(c);
    circleButton(mixBtn(c, 0), "M", gMute[c] ? kRed : kBtn);
    if (c < 3) {
      circleButton(mixBtn(c, 1), "S", gSolo[c] ? kYellow : kBtn, gSolo[c] ? kBg : kWhite);
    } else {
      // Main out has no Solo (soloing the final mix is meaningless), that
      // spot is instead a toggle for which input the stutter grid acts on,
      // mirroring Config's Stutter Track setting. Hollow so it reads
      // differently from the solid mute/limiter/rec buttons around it.
      // Shows the same 3 letter label as the column it targets (kStutTrack
      // order is Looper, Synth, Main, Aux; those are the LOP, INT, ALL, EXT
      // columns respectively), not an unrelated abbreviation.
      static const int kStutCol[4] = {2, 0, 3, 1};
      circleButtonHollow(mixBtn(c, 1), names[kStutCol[cfgStutTrack]], kOrange, kOrange, 1);
    }
    char lim[8];
    snprintf(lim, sizeof(lim), "%d", -limiterIdx[c]);
    circleButton(mixBtn(c, 2), lim, limiterIdx[c] ? kBlue : kBtn);
    if (c < 2) {
      const Rect r = mixBtn(c, 3);
      const uint16_t f = gRecEnable[c] ? kOrange : kBtn;
      d.fillCircle(r.x + r.w / 2, r.y + r.h / 2, r.w / 2, f);
      icon(I_LOOP, r.x + r.w / 2, r.y + r.h / 2, 0, kWhite, f);
    } else if (c == 3) {
      circleButton(mixBtn(c, 3), "PMP", compOn ? kOrange : kBtn, kWhite, 1);
    }
    text(names[c], colX(c) + 58, 229, 2, anySolo && c < 3 && !gSolo[c] ? kGrey : kWhite, kBg);
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
    if (amy[i].chan == 10) {
      snprintf(b, sizeof(b), "Drums %d", amy[i].patch);
    } else {
      const char *nm = kPatchNames[amy[i].patch][0] ? kPatchNames[amy[i].patch] : "(unnamed)";
      truncateToWidth(nm, 2, r.w - 12 - 8, b, sizeof(b));  // box width minus left/right margin
    }
    text(b, r.x + 12, r.y + 62, 2, kGrey, kDark, false);
    snprintf(b, sizeof(b), "Vol %d", amy[i].vol);
    text(b, r.x + 12, r.y + 92, 2, kGrey, kDark, false);
  }
}

void paramValue(const Param &p, char *out, size_t n) {
  if (p.names)
    snprintf(out, n, "%s", p.names[*p.value - p.lo]);
  else
    snprintf(out, n, "%d", *p.value);
}

// Parameter list with a page scroll bar on the right. showX draws the back X.
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
  button({288, 34, 30, 32}, "", kBtn);
  triangle(303, 50, 8, true, page > 0 ? kWhite : kGrey);
  button({288, 202, 30, 32}, "", kBtn);
  triangle(303, 218, 8, false, page < pages - 1 ? kWhite : kGrey);
  const int segH = 130 / pages;
  for (int s = 0; s < pages; s++)
    d.fillRect(288, 68 + s * segH + 1, 30, segH - 2, s == page ? kBrightGreen : kBtn);
}

// Patch picking is a category menu first (Juno-6 / DX-7 / Drum Kit /
// Custom / SD Card), matching the taxonomy AMY's own patch table actually
// has, rather than one flat 391 entry list: a plain page dot strip
// (drawList's approach) breaks down past ~65 pages (130/pages rounds to
// nothing), and scrolling through DX7 patches to find a Juno one, or vice
// versa, was never a good experience anyway. Custom and SD Card are shown
// (greyed, inert) but not wired up yet, see FIRMWARE_PLAN.md's Synth and
// sampler section for that backlog, not dropped, just not built.
enum PatchCat { CAT_JUNO, CAT_DX7, CAT_DRUM, CAT_CUSTOM, CAT_SD, CAT_COUNT };
const char *const kPatchCatNames[CAT_COUNT] = {"Juno-6", "DX-7", "Drum Kit", "Custom", "SD Card"};
const bool kPatchCatEnabled[CAT_COUNT] = {true, true, true, false, false};
// Curated, not a range: AMY's patch table's real standalone kits are named
// "MIDI drums ..." / "drum kit N ..." specifically, a handful of Juno/DX7
// patches also have "drum" in their own descriptive name (Steel Drums,
// LOG DRUM, ...) without being a kit, so this is hand picked from
// patch_names.h rather than pattern matched.
const int kDrumKitPatches[] = {258, 384, 385, 386, 387, 388, 389, 390};
constexpr int kDrumKitCount = sizeof(kDrumKitPatches) / sizeof(kDrumKitPatches[0]);
int patchCat = -1;  // -1 = category menu itself, else which category's list is open

int catCount(int cat) {
  switch (cat) {
    case CAT_JUNO: return 128;
    case CAT_DX7: return 128;
    case CAT_DRUM: return kDrumKitCount;
    default: return 0;
  }
}
int catPatchIndex(int cat, int row) {
  switch (cat) {
    case CAT_JUNO: return row;
    case CAT_DX7: return 128 + row;
    case CAT_DRUM: return kDrumKitPatches[row];
    default: return -1;
  }
}
int catOf(int patchIdx) {
  if (patchIdx >= 0 && patchIdx < 128) return CAT_JUNO;
  if (patchIdx >= 128 && patchIdx < 256) return CAT_DX7;
  for (int i = 0; i < kDrumKitCount; i++)
    if (kDrumKitPatches[i] == patchIdx) return CAT_DRUM;
  return -1;
}

void drawPatchPicker() {
  auto &d = M5.Display;
  d.fillScreen(kBg);
  button({0, 0, 44, 30}, "X", kRed, kWhite, 2);
  if (patchCat < 0) {
    text("PATCH", 70, 15, 2, kWhite, kBg, false);
    const int curCat = amyEdit >= 0 ? catOf(amy[amyEdit].patch) : -1;
    for (int c = 0; c < CAT_COUNT; c++) {
      const Rect r = {4, 34 + c * 41, 314, 38};
      d.fillRoundRect(r.x, r.y, r.w, r.h, 6, kDark);
      text(kPatchCatNames[c], r.x + 8, r.y + 19, 2, kPatchCatEnabled[c] ? kWhite : kGrey, kDark,
           false);
      if (c == curCat) {
        const int ax = r.x + r.w - 20, ay = r.y + r.h / 2;
        d.fillTriangle(ax - 6, ay - 8, ax - 6, ay + 8, ax + 6, ay, kBrightGreen);
      }
    }
    return;
  }
  text(kPatchCatNames[patchCat], 70, 15, 2, kWhite, kBg, false);
  const int n = catCount(patchCat);
  const int pages = (n + kRowsPerPage - 1) / kRowsPerPage;
  const int cur = amyEdit >= 0 ? amy[amyEdit].patch : -1;
  for (int i = 0; i < kRowsPerPage; i++) {
    const int row = patchPickerPage * kRowsPerPage + i;
    if (row >= n) break;
    const int idx = catPatchIndex(patchCat, row);
    const Rect r = {4, 34 + i * 41, 278, 38};
    d.fillRoundRect(r.x, r.y, r.w, r.h, 6, idx == cur ? kBlue : kDark);
    char b[40];
    const char *nm = kPatchNames[idx][0] ? kPatchNames[idx] : "(unnamed)";
    snprintf(b, sizeof(b), "%d: %s", idx, nm);
    text(b, r.x + 8, r.y + 19, 2, kWhite, idx == cur ? kBlue : kDark, false);
  }
  // Same page arrows and segmented index strip as drawList's, safe here:
  // the biggest category (Juno-6/DX-7, 128 patches) is 26 pages, nowhere
  // near where that strip's math breaks down.
  button({288, 34, 30, 32}, "", kBtn);
  triangle(303, 50, 8, true, patchPickerPage > 0 ? kWhite : kGrey);
  button({288, 202, 30, 32}, "", kBtn);
  triangle(303, 218, 8, false, patchPickerPage < pages - 1 ? kWhite : kGrey);
  const int segH = 130 / pages;
  for (int s = 0; s < pages; s++)
    d.fillRect(288, 68 + s * segH + 1, 30, segH - 2, s == patchPickerPage ? kBrightGreen : kBtn);
}

void pressPatchPicker(int x, int y) {
  if (Rect{0, 0, 44, 30}.hit(x, y)) {
    // Back one screen: out of a category's list to the category menu, or
    // out of the category menu to the AMY channel's param list.
    if (patchCat >= 0) {
      patchCat = -1;
    } else {
      patchPicker = false;
    }
    dirty = true;
    return;
  }
  if (patchCat < 0) {
    for (int c = 0; c < CAT_COUNT; c++) {
      if (!kPatchCatEnabled[c]) continue;
      if (!Rect{4, 34 + c * 41, 314, 38}.hit(x, y)) continue;
      patchCat = c;
      // Land on the current patch's page if it is in this category,
      // otherwise start at the top rather than wherever it was left.
      const int cur = amyEdit >= 0 ? amy[amyEdit].patch : -1;
      patchPickerPage = 0;
      if (catOf(cur) == c) {
        for (int row = 0; row < catCount(c); row++) {
          if (catPatchIndex(c, row) == cur) {
            patchPickerPage = row / kRowsPerPage;
            break;
          }
        }
      }
      dirty = true;
      return;
    }
    return;
  }
  const int n = catCount(patchCat);
  const int pages = (n + kRowsPerPage - 1) / kRowsPerPage;
  if (Rect{288, 34, 30, 32}.hit(x, y) && patchPickerPage > 0) patchPickerPage--, dirty = true;
  if (Rect{288, 202, 30, 32}.hit(x, y) && patchPickerPage < pages - 1) patchPickerPage++, dirty = true;
  if (x >= 288 && y >= 68 && y < 198) {
    const int s = (y - 68) / (130 / pages);
    if (s < pages) patchPickerPage = s, dirty = true;
  }
  for (int i = 0; i < kRowsPerPage; i++) {
    const int row = patchPickerPage * kRowsPerPage + i;
    if (row >= n) break;
    if (!Rect{4, 34 + i * 41, 278, 38}.hit(x, y)) continue;
    if (amyEdit >= 0) amy[amyEdit].patch = catPatchIndex(patchCat, row);
    patchPicker = false;
    patchCat = -1;
    dirty = true;
    return;
  }
}

// MIDI Chan's row opens this instead of the punch in editor: 16 is a
// small, fixed set, a straight 4x4 grid picks one in a single tap rather
// than stepping a number up/down or typing it.
void drawChanPicker() {
  auto &d = M5.Display;
  d.fillScreen(kBg);
  button({0, 0, 44, 30}, "X", kRed, kWhite, 2);
  text("MIDI CHAN", 70, 15, 2, kWhite, kBg, false);
  const int cur = amyEdit >= 0 ? amy[amyEdit].chan : -1;
  for (int ch = 1; ch <= 16; ch++) {
    const int col = (ch - 1) % 4, row = (ch - 1) / 4;
    char b[4];
    snprintf(b, sizeof(b), "%d", ch);
    button({4 + col * 78, 40 + row * 49, 74, 45}, b, ch == cur ? kBlue : kBtn, kWhite, 3);
  }
}

void pressChanPicker(int x, int y) {
  if (Rect{0, 0, 44, 30}.hit(x, y)) {
    chanPicker = false;
    dirty = true;
    return;
  }
  for (int ch = 1; ch <= 16; ch++) {
    const int col = (ch - 1) % 4, row = (ch - 1) / 4;
    if (!Rect{4 + col * 78, 40 + row * 49, 74, 45}.hit(x, y)) continue;
    if (amyEdit >= 0) amy[amyEdit].chan = ch;
    chanPicker = false;
    dirty = true;
    return;
  }
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

  const looper::State ls = looper::state();
  const Rect stopR = {163, 123, 74, 114};
  const Rect playR = {243, 123, 74, 114};

  // Stop button: pause while playing (data exists), square while recording or
  // armed (no data yet), clear when stopped, undo when cleared.
  Icon si = I_STOP;
  const char *scap = "STOP";
  uint16_t sfill = kBtn;
  switch (ls) {
    case looper::PLAY: si = I_PAUSE; scap = "PAUSE"; break;
    case looper::REC:
    case looper::ARMED: si = I_STOP; scap = "STOP"; break;
    case looper::STOP: si = I_CLEAR; scap = "CLEAR"; break;
    case looper::EMPTY:
      si = I_UNDO;
      scap = "UNDO";
      if (!looper::undoAvailable()) sfill = kDark;
      break;
  }
  d.fillRoundRect(stopR.x, stopR.y, stopR.w, stopR.h, 6, sfill);
  icon(si, stopR.x + 37, stopR.y + 44, 20, kWhite, sfill);
  text(scap, stopR.x + 37, stopR.y + 96, 1, kWhite, sfill);

  Icon pi = I_ARM;
  const char *pcap = "ARM";
  uint16_t pfill = kBtn;
  switch (ls) {
    case looper::PLAY:
      pi = I_REC;
      pcap = "OVERDUB";
      pfill = looper::overdubbing() ? kRed : kBtn;
      break;
    case looper::STOP:
      pi = I_PLAY;
      pcap = "PLAY";
      pfill = kGreen;
      break;
    case looper::EMPTY:
      pi = cfgTm ? I_REC : I_ARM;
      pcap = cfgTm ? "CAPTURE" : "ARM";
      pfill = millis() < redUntil ? kRed : kBtn;
      break;
    case looper::ARMED:
      pi = I_REC;
      pcap = "REC NOW";
      pfill = kOrange;
      break;
    case looper::REC:
      pi = I_REC;
      pcap = "RECORDING";
      pfill = kRed;
      break;
  }
  d.fillRoundRect(playR.x, playR.y, playR.w, playR.h, 6, pfill);
  icon(pi, playR.x + 37, playR.y + 44, 20, kWhite, pfill);
  text(pcap, playR.x + 37, playR.y + 96, 1, kWhite, pfill);
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
  // 11 keys (1-9, C, 0), not 12: the small OK tile that used to sit in the
  // last grid slot is gone, the big OK to the right (below) is the only
  // one now, that slot is just left blank.
  static const char *pad[11] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "C", "0"};
  for (int i = 0; i < 11; i++)
    button({(i % 3) * 66 + 2, 88 + (i / 3) * 38, 62, 35}, pad[i], kBtn, kWhite, 2);
  button({208, 34, 108, 52}, "", kBtn);
  triangle(262, 60, 12, true, kWhite);
  button({208, 92, 108, 52}, "", kBtn);
  triangle(262, 118, 12, false, kWhite);
  if (ed.tap) button({208, 150, 108, 52}, "TAP", kOrange, kWhite, 2);
  // Standard on every number pad screen: a big OK filling whatever's left
  // of the right column below Up/Down (and Tap, when present), not just
  // the small OK tile in the digit grid.
  const int okY = ed.tap ? 202 : 150;
  button({208, okY, 108, 240 - okY}, "OK", kGreen, kWhite, 3);
}

void redraw() {
  spi_lock::Guard lock;
  if (ed.open) return drawEditor();
  switch (screen) {
    case S_MIXER: return drawMixer();
    case S_AMY:
      if (patchPicker) return drawPatchPicker();
      if (chanPicker) return drawChanPicker();
      if (amyEdit >= 0) {
        char t[16];
        snprintf(t, sizeof(t), "CH %d", amy[amyEdit].chan);
        return drawList(amyParams, kAmyParamsN, t, 0, true);
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
  for (int i = 0; i < 11; i++) {
    if (!Rect{(i % 3) * 66 + 2, 88 + (i / 3) * 38, 62, 35}.hit(x, y)) continue;
    if (i == 9) {
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
  const int okY = ed.tap ? 202 : 150;
  if (Rect{208, okY, 108, 240 - okY}.hit(x, y)) ed.open = false, dirty = true;
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

int dragCol = -1;

void pressMixer(int x, int y) {
  for (int c = 0; c < 4; c++) {
    if (mixBtn(c, 0).hit(x, y)) gMute[c] = !gMute[c], dirty = true;
    if (c < 3 && mixBtn(c, 1).hit(x, y)) gSolo[c] = !gSolo[c], dirty = true;
    if (c == 3 && mixBtn(c, 1).hit(x, y)) cfgStutTrack = (cfgStutTrack + 1) % 4, dirty = true;
    if (mixBtn(c, 2).hit(x, y)) limiterIdx[c] = (limiterIdx[c] + 1) % 7, dirty = true;
    if (mixBtn(c, 3).hit(x, y)) {
      if (c < 2)
        gRecEnable[c] = !gRecEnable[c];
      else if (c == 3)
        compOn = !compOn;
      dirty = true;
    }
  }
}

void pressLooper(int x, int y) {
  for (int i = 0; i < 4; i++)
    if (Rect{i * 80 + 3, 3, 74, 114}.hit(x, y)) slot = i, dirty = true;
  if (Rect{3, 123, 74, 114}.hit(x, y)) openEditor("BPM", &bpm, 20, 300, true);
  if (Rect{83, 123, 74, 114}.hit(x, y)) openEditor("Measures", &measures, 1, 8, false);
  const looper::State ls = looper::state();
  if (Rect{163, 123, 74, 114}.hit(x, y)) {
    switch (ls) {
      case looper::PLAY:
      case looper::REC:
      case looper::ARMED: looper::command(looper::C_STOP); break;
      case looper::STOP: looper::command(looper::C_CLEAR); break;
      case looper::EMPTY: looper::command(looper::C_UNDO); break;
    }
  }
  if (Rect{243, 123, 74, 114}.hit(x, y)) {
    switch (ls) {
      case looper::PLAY: looper::command(looper::C_OVERDUB); break;
      case looper::STOP: looper::command(looper::C_PLAY); break;
      case looper::EMPTY: looper::command(cfgTm ? looper::C_CAPTURE : looper::C_ARM); break;
      case looper::ARMED: looper::command(looper::C_REC_NOW); break;
      case looper::REC: looper::command(looper::C_END_REC); break;
    }
  }
}

void pressAmy(int x, int y) {
  if (patchPicker) return pressPatchPicker(x, y);
  if (chanPicker) return pressChanPicker(x, y);
  if (amyEdit >= 0) {
    // MIDI Chan (amyParams[0]) opens the 16 button grid, Patch
    // (amyParams[1]) opens the named picker, both instead of the usual
    // punch in editor. Everything else on this screen still uses
    // pressList as normal.
    if (Rect{4, 34, 278, 38}.hit(x, y)) {
      chanPicker = true;
      dirty = true;
      return;
    }
    if (Rect{4, 34 + 41, 278, 38}.hit(x, y)) {
      patchCat = -1;  // always opens on the category menu, not a list
      patchPicker = true;
      dirty = true;
      return;
    }
    return pressList(amyParams, kAmyParamsN, amyPage, x, y, true);
  }
  for (int i = 0; i < 4; i++) {
    if (!Rect{(i % 2) * 160 + 3, (i / 2) * 120 + 3, 154, 114}.hit(x, y)) continue;
    amyEdit = i;
    amyPage = 0;
    amyParams[0] = {"MIDI Chan", &amy[i].chan, 1, 16, nullptr};
    amyParams[1] = {"Patch", &amy[i].patch, 0, kPatchNameCount - 1, nullptr};
    amyParams[2] = {"Volume", &amy[i].vol, 0, 100, nullptr};
    amyParams[3] = {"Voices", &amy[i].voices, 1, 16, nullptr};
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
    float v = 1.0f - (float)(y - kTrackW / 2) / (kTrackH - kTrackW);
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

void pushLooperParams() {
  looper::params.bpm = bpm;
  looper::params.measures = measures;
  looper::params.beats = cfgSig == 0 ? 4 : 3;
  looper::params.timeMachine = cfgTm;
  looper::params.tmGapBeats = cfgTmGap;
  looper::params.armNow = cfgRecStart == 1;
  looper::params.autoOverdub = cfgAutoOd;
}

void pushStutter() { stutter::set(stutHeld, cfgStutTrack); }

void applyAmy() {
  for (int i = 0; i < 4; i++) {
    if (amy[i].chan != amyChanSeen[i]) {
      // to_synth moves the synth to a new MIDI channel number, addressed by
      // its current number (amySynthId[i]) before the move.
      synth_engine::setChannel(amySynthId[i], amy[i].chan);
      amySynthId[i] = amy[i].chan;
      amyChanSeen[i] = amy[i].chan;
    }
    if (amy[i].patch != amySeen[i].patch) {
      amySeen[i].patch = amy[i].patch;
      if (amySynthId[i] != 10) synth_engine::setPatch(amySynthId[i], amy[i].patch, amy[i].voices);
    }
    if (amy[i].vol != amySeen[i].vol) {
      amySeen[i].vol = amy[i].vol;
      synth_engine::setLevel(amySynthId[i], amy[i].vol / 100.0f);
    }
    if (amy[i].voices != amySeen[i].voices) {
      amySeen[i].voices = amy[i].voices;
      if (amySynthId[i] != 10) synth_engine::setVoices(amySynthId[i], amy[i].voices);
    }
  }
}

// Settings persistence (NVS, via Preferences). One fixed-size POD struct
// written/read as raw bytes rather than dozens of individual keys, magic
// and version guard against reading a struct shaped differently than this
// build expects (a future field added mid-struct would otherwise silently
// misread every field after it) rather than corrupting settings quietly.
// Debounced rather than hooked into every mute/drag/edit call site: tick()
// diffs a fresh snapshot against the last saved one every 3s and only
// writes when something actually changed, so a fader drag or a held mixer
// screen doesn't hammer flash, and no mutation site can be missed.
constexpr uint32_t kSettingsMagic = 0x4C4F4F50;  // 'LOOP'
constexpr uint16_t kSettingsVersion = 1;
struct PersistedSettings {
  uint32_t magic = kSettingsMagic;
  uint16_t version = kSettingsVersion;
  float gLevelP[4];
  bool gMuteP[4];
  bool gSoloP[3];
  bool gRecEnableP[2];
  int limiterIdxP[4];
  bool compOnP;
  int cfgRecStartP, cfgSigP, cfgAutoOdP, cfgTmP, cfgTmGapP, cfgAudioOutP, cfgSdRecP, cfgBpmMidiP,
      cfgStutTrackP;
  int cfgIntMaxGainDbP, cfgExtMaxGainDbP;
  int amyChanP[4], amyPatchP[4], amyVolP[4], amyVoicesP[4];
  int bpmP, measuresP;
};
Preferences prefs;
PersistedSettings lastSaved;      // what's actually on flash right now
PersistedSettings pendingSaved;   // most recent live snapshot, may still be moving
uint32_t lastSettingsCheck = 0;
uint32_t pendingSettingsSince = 0;  // when pendingSaved last actually changed

PersistedSettings buildSettings() {
  PersistedSettings s;
  for (int i = 0; i < 4; i++) s.gLevelP[i] = gLevel[i];
  for (int i = 0; i < 4; i++) s.gMuteP[i] = gMute[i];
  for (int i = 0; i < 3; i++) s.gSoloP[i] = gSolo[i];
  for (int i = 0; i < 2; i++) s.gRecEnableP[i] = gRecEnable[i];
  for (int i = 0; i < 4; i++) s.limiterIdxP[i] = limiterIdx[i];
  s.compOnP = compOn;
  s.cfgRecStartP = cfgRecStart;
  s.cfgSigP = cfgSig;
  s.cfgAutoOdP = cfgAutoOd;
  s.cfgTmP = cfgTm;
  s.cfgTmGapP = cfgTmGap;
  s.cfgAudioOutP = cfgAudioOut;
  s.cfgSdRecP = cfgSdRec;
  s.cfgBpmMidiP = cfgBpmMidi;
  s.cfgStutTrackP = cfgStutTrack;
  s.cfgIntMaxGainDbP = cfgIntMaxGainDb;
  s.cfgExtMaxGainDbP = cfgExtMaxGainDb;
  for (int i = 0; i < 4; i++) {
    s.amyChanP[i] = amy[i].chan;
    s.amyPatchP[i] = amy[i].patch;
    s.amyVolP[i] = amy[i].vol;
    s.amyVoicesP[i] = amy[i].voices;
  }
  s.bpmP = bpm;
  s.measuresP = measures;
  return s;
}

void applySettings(const PersistedSettings &s) {
  for (int i = 0; i < 4; i++) gLevel[i] = s.gLevelP[i];
  for (int i = 0; i < 4; i++) gMute[i] = s.gMuteP[i];
  for (int i = 0; i < 3; i++) gSolo[i] = s.gSoloP[i];
  for (int i = 0; i < 2; i++) gRecEnable[i] = s.gRecEnableP[i];
  for (int i = 0; i < 4; i++) limiterIdx[i] = s.limiterIdxP[i];
  compOn = s.compOnP;
  cfgRecStart = s.cfgRecStartP;
  cfgSig = s.cfgSigP;
  cfgAutoOd = s.cfgAutoOdP;
  cfgTm = s.cfgTmP;
  cfgTmGap = s.cfgTmGapP;
  cfgAudioOut = s.cfgAudioOutP;
  cfgSdRec = s.cfgSdRecP;
  cfgBpmMidi = s.cfgBpmMidiP;
  cfgStutTrack = s.cfgStutTrackP;
  cfgIntMaxGainDb = s.cfgIntMaxGainDbP;
  cfgExtMaxGainDb = s.cfgExtMaxGainDbP;
  for (int i = 0; i < 4; i++) {
    amy[i].chan = s.amyChanP[i];
    amy[i].patch = s.amyPatchP[i];
    amy[i].vol = s.amyVolP[i];
    amy[i].voices = s.amyVoicesP[i];
    // Force applyAmy()'s next pass to push every field for every channel:
    // it only sends an AMY event where amy[i] differs from amySeen[i]/
    // amyChanSeen[i], and both default to the same values amy[i] itself
    // defaults to, so without this a loaded value that happens to match a
    // fresh boot's default would silently never reach AMY.
    amySeen[i] = {-1, -1, -1, -1};
    amyChanSeen[i] = -1;
  }
  bpm = s.bpmP;
  measures = s.measuresP;
}

void loadSettings() {
  prefs.begin("loopanini", /*readOnly=*/false);
  PersistedSettings s;
  const size_t got = prefs.getBytes("settings", &s, sizeof(s));
  if (got == sizeof(s) && s.magic == kSettingsMagic && s.version == kSettingsVersion) {
    applySettings(s);
  }
  // Either way, lastSaved/pendingSaved start as whatever is actually live
  // right now (loaded values, or the compiled in defaults on a first boot
  // / version bump), so the first tick() doesn't immediately re-save a
  // no-op.
  lastSaved = buildSettings();
  pendingSaved = lastSaved;
}

// True quiet period debounce, not "write every N seconds while dirty":
// a live tweak (a fader drag, an effects parameter once those exist) can
// keep moving for several seconds straight, and an NVS write taking the
// flash bus mid gesture is a real, audible risk, not just wear. Checked
// cheaply and often (memcmp of one ~150 byte struct), only actually
// written to flash once nothing has changed for a full 5s.
constexpr uint32_t kSettingsQuietMs = 5000;
void maybeSaveSettings(uint32_t now) {
  if (now - lastSettingsCheck < 250) return;
  lastSettingsCheck = now;
  const PersistedSettings current = buildSettings();
  if (memcmp(&current, &pendingSaved, sizeof(PersistedSettings)) != 0) {
    pendingSaved = current;
    pendingSettingsSince = now;
    return;
  }
  if (now - pendingSettingsSince < kSettingsQuietMs) return;
  if (memcmp(&pendingSaved, &lastSaved, sizeof(PersistedSettings)) == 0) return;
  prefs.putBytes("settings", &pendingSaved, sizeof(pendingSaved));
  lastSaved = pendingSaved;
}

void tick(uint32_t now) {
  if (now - lastTick < 40) return;
  lastTick = now;
  pushLooperParams();
  applyAmy();
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

  const uint32_t rj = looper::rejectCount();
  if (rj != seenRejects) {
    seenRejects = rj;
    redUntil = now + 600;
    dirty = true;
  }
  if (redUntil && now >= redUntil) {
    redUntil = 0;
    dirty = true;
  }
  const looper::State ls = looper::state();
  if (ls != seenState || looper::overdubbing() != seenOd || looper::undoAvailable() != seenUndo) {
    seenState = ls;
    seenOd = looper::overdubbing();
    seenUndo = looper::undoAvailable();
    dirty = true;
  }

  // The Mixer screen's meters used to redraw and pushSprite() all 4 columns
  // every tick (25/s) whenever idling on this screen, unconditionally, real
  // signal or not. That is up to 100 SPI transactions a second just from
  // sitting on this screen, on top of whatever touch elsewhere triggers,
  // and hardware audio testing traced pops specifically to being on this
  // screen with a channel unmoving/muted staying quiet, matching this
  // exact codepath: less SPI traffic here, less chance of colliding with
  // the I2S timing the audio task depends on. Slowed to 10/s (adequate for
  // a VU meter, a real drop from 25) and skips any column whose displayed
  // level hasn't moved enough to look different, rather than pushing a
  // pixel-identical sprite over SPI again.
  if (!dirty && screen == S_MIXER && !ed.open && now - lastMeterDraw >= 100) {
    lastMeterDraw = now;
    spi_lock::Guard lock;
    for (int c = 0; c < 4; c++) {
      const bool moved = fabsf(dispLevel[c] - meterSeen[c]) >= 0.01f ||
                          fabsf(gLevel[c] - faderSeen[c]) >= 0.004f || gMute[c] != muteSeen[c];
      if (!moved) continue;
      meterSeen[c] = dispLevel[c];
      faderSeen[c] = gLevel[c];
      muteSeen[c] = gMute[c];
      drawTrack(c);
    }
  }
  maybeSaveSettings(now);
}

}  // namespace

// ---------------------------------------------------------------- API ----

void begin() {
  looper::begin();
  stutter::begin();
  track.createSprite(kTrackW, kTrackH);
  loadSettings();
  dirty = true;
}

void update() {
  M5.update();
  const uint32_t now = millis();
  const auto t = M5.Touch.getDetail();
  if (t.wasPressed() && t.y < kStripY) onPress(t.x, t.y);
  if (t.isPressed() && t.y < kStripY) whileHeld(t.x, t.y);
  if (t.wasReleased()) {
    if (t.base_y >= kStripY) {
      // Bottom nav always means "leave to the next/prev main screen", even
      // from inside a box with its own red X (the numeric editor, an AMY
      // channel's edit view, the patch/channel pickers): close whichever
      // of those is open first, same as tapping its own X, then navigate
      // exactly as if we'd been at the top level the whole time, rather
      // than swallowing the gesture silently.
      ed.open = false;
      amyEdit = -1;
      patchPicker = false;
      chanPicker = false;
      const int dx = t.x - t.base_x;
      if (dx > 40)
        gotoScreen(-1);
      else if (dx < -40)
        gotoScreen(1);
      else
        gotoScreen(t.base_x < 160 ? -1 : 1);
    }
    if (stutHeld >= 0) stutHeld = -1, dirty = true;
    dragCol = -1;
  }
  pushStutter();
  tick(now);
  if (dirty) {
    dirty = false;
    redraw();
  }
}

// SP1LimiterJS constants, verbatim from the source: c converts the
// Threshold dB slider to a linear ratio (thresh = exp(dB / c)), the pole
// gives the envelope's one pole lowpass its 10Hz corner (2*pi*10 =
// 62.83185307). dc is a tiny numerical floor so t never sqrt()'s a
// slightly negative value from float rounding, negligible at int16 scale.
constexpr float kSp1C = 8.65617025f;
constexpr float kSp1Dc = 1e-30f;

// One call per CHANNEL PER BLOCK (not per frame, see the 2026-09-24
// thirteenth pass note): st.t is that channel's envelope filter state,
// persists block to block. blockPeak is the loudest |sample| anywhere in
// this block for this channel (caller's cheap pre-pass, no sqrt, no
// state). thresh is limiterIdx's dB converted via kSp1C, left as the
// source's own 0..1 normalized ratio, NOT scaled to int16 magnitude:
// peak/rms are normalized to 0..1 here (divide by 32768) precisely so
// they compare against thresh in the same units the source does, then the
// dimensionless gain that falls out divides cleanly into an int16 sample
// at the call site. Scaling thresh up to int16 magnitude instead (tried
// first, caught before shipping) makes gain itself thousands instead of
// roughly unity, which crushes everything toward silence rather than
// riding it up to the threshold. poleB is -exp(-2*pi*10/srate), constant
// unless the sample rate changes.
// Returns the gain to DIVIDE the whole block by, not multiply: below
// threshold that is gain=thresh, a fixed divisor less than 1, so quiet
// input comes out louder, automatic makeup gain, the behavior this project
// wanted from the LOSER pack's other limiter that MGA_JSLimiter (tried
// first) does not have. Above threshold gain=the envelope itself, which
// divides the block's peak back down to exactly unity, self bounding by
// construction, same reasoning as the source. Applying one gain across a
// whole 256 sample block instead of a fresh one every sample quantizes
// the envelope to block granularity (~5.3ms at 48kHz), well under the
// 10Hz/~100ms filter's own time constant, so it is not an audible
// tradeoff, unlike calling sqrtf() 256 times a block was turning out to be.
float limiterGain(LimiterState &st, float blockPeak, float thresh, float poleB) {
  const float peak = blockPeak / 32768.0f;
  const float a = 1.0f + poleB;
  st.t = a * peak - poleB * st.t + kSp1Dc;
  float smoothed = st.t - kSp1Dc;
  smoothed = smoothed > 0.0f ? sqrtf(smoothed) : 0.0f;
  const float rms = fmaxf(smoothed, peak);
  return rms > thresh ? rms : thresh;
}

void limiterCoefs(int ch, float &thresh, float &poleB) {
  thresh = expf(-(float)limiterIdx[ch] / kSp1C);
  poleB = -expf(-62.83185307f / LOOPANINI_SAMPLE_RATE);
}

// LOP is already int16, so it can run the limiter as a standalone pass
// after the fact rather than inline like INT/EXT/Main below. Two passes:
// a cheap peak scan, then one limiterGain() call, then a flat divide, see
// limiterGain's comment for why this replaced a per-sample call.
void applyLimiter(int16_t *buf, int frames, int ch) {
  float threshLinear, poleB;
  limiterCoefs(ch, threshLinear, poleB);
  float blockPeak = 0.0f;
  for (int i = 0; i < frames * 2; i++) {
    const float a = fabsf((float)buf[i]);
    if (a > blockPeak) blockPeak = a;
  }
  const float gain = limiterGain(limSt[ch], blockPeak, threshLinear, poleB);
  for (int i = 0; i < frames * 2; i++) {
    float v = buf[i] / gain;
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    buf[i] = (int16_t)v;
  }
}

void processBlock(int16_t *b, const int16_t *aux, int frames) {
  const bool anySolo = gSolo[0] || gSolo[1] || gSolo[2];
  auto silent = [&](int c) { return gMute[c] || (anySolo && !gSolo[c]); };
  const float intMaxGain = powf(10.0f, (float)cfgIntMaxGainDb / 20.0f);
  const float extMaxGain = powf(10.0f, (float)cfgExtMaxGainDb / 20.0f);
  const float g0 = silent(0) ? 0.0f : gLevel[0] * gLevel[0] * intMaxGain;
  const float g1 = silent(1) ? 0.0f : gLevel[1] * gLevel[1] * extMaxGain;
  const float g2 = silent(2) ? 0.0f : gLevel[2] * gLevel[2];
  const float g3 = gMute[3] ? 0.0f : gLevel[3] * gLevel[3];
  // INT/EXT/Main run their limiter inline below, right on the post fader
  // float (after Max Gain), not as a separate pass afterward: SP1's gain divides, and above
  // threshold it self bounds to int16 magnitude by construction (see
  // limiterGain's comment), so there is nothing to preemptively hard clamp
  // before it runs, unlike the max gain boost this project tried and
  // removed. LOP has no fader boost either, so applyLimiter (above) working
  // from its already-int16 output is fine as a standalone pass.
  float thresh0, pole0, thresh1, pole1, thresh3, pole3;
  limiterCoefs(0, thresh0, pole0);
  limiterCoefs(1, thresh1, pole1);
  limiterCoefs(3, thresh3, pole3);

  static int16_t extSig[2 * 1024];
  static int16_t loopIn[2 * 1024];
  static int16_t loopOut[2 * 1024];
  if (frames > 1024) frames = 1024;

  // Cheap peak-only pre-pass (no sqrt, no filter state) so limiterGain()
  // runs once per channel for the whole block, not once per sample, see
  // its comment. b[i]/aux[i] get re-scaled by g0/g1 again below, that
  // multiply is not what was expensive here.
  float peak0 = 0.0f, peak1 = 0.0f;
  for (int i = 0; i < frames * 2; i++) {
    const float a0 = fabsf(b[i] * g0);
    if (a0 > peak0) peak0 = a0;
    const float a1 = fabsf(aux[i] * g1);
    if (a1 > peak1) peak1 = a1;
  }
  const float gain0 = limiterGain(limSt[0], peak0, thresh0, pole0);
  const float gain1 = limiterGain(limSt[1], peak1, thresh1, pole1);

  int p0 = 0, p1 = 0;
  for (int i = 0; i < frames; i++) {
    float l0 = b[2 * i] * g0, r0v = b[2 * i + 1] * g0;
    float l1 = aux[2 * i] * g1, r1v = aux[2 * i + 1] * g1;
    l0 /= gain0;
    r0v /= gain0;
    l1 /= gain1;
    r1v /= gain1;
    // Final safety clamp: float/int16 boundary insurance, not the real gain
    // control, see limiterGain's comment on why this should rarely trigger.
    if (l0 > 32767.0f) l0 = 32767.0f;
    if (l0 < -32768.0f) l0 = -32768.0f;
    if (r0v > 32767.0f) r0v = 32767.0f;
    if (r0v < -32768.0f) r0v = -32768.0f;
    if (l1 > 32767.0f) l1 = 32767.0f;
    if (l1 < -32768.0f) l1 = -32768.0f;
    if (r1v > 32767.0f) r1v = 32767.0f;
    if (r1v < -32768.0f) r1v = -32768.0f;
    const int a0 = (int)fmaxf(fabsf(l0), fabsf(r0v));
    const int a1 = (int)fmaxf(fabsf(l1), fabsf(r1v));
    if (a0 > p0) p0 = a0;
    if (a1 > p1) p1 = a1;
    b[2 * i] = (int16_t)l0;         // INT, post fader/mute/solo/limiter
    b[2 * i + 1] = (int16_t)r0v;
    extSig[2 * i] = (int16_t)l1;    // EXT, post fader/mute/solo/limiter
    extSig[2 * i + 1] = (int16_t)r1v;
    // Looper input: whichever of INT/EXT are record-enabled, summed. Taken
    // before stutter is applied to either, so a stutter repeat is never
    // recorded into the loop, same as arpnmidi's Stutter.
    for (int s = 0; s < 2; s++) {
      int32_t mix = 0;
      if (gRecEnable[0]) mix += b[2 * i + s];
      if (gRecEnable[1]) mix += extSig[2 * i + s];
      loopIn[2 * i + s] = (int16_t)(mix > 32767 ? 32767 : (mix < -32768 ? -32768 : mix));
    }
  }
  const bool recording = gRecEnable[0] || gRecEnable[1];
  const float p2 = looper::process(loopIn, frames, recording, g2, loopOut);

  stutter::apply(b, frames, stutter::T_SYNTH);
  stutter::apply(extSig, frames, stutter::T_AUX);
  applyLimiter(loopOut, frames, 2);
  stutter::apply(loopOut, frames, stutter::T_LOOPER);

  float peak3 = 0.0f;
  for (int i = 0; i < frames * 2; i++) {
    const float a3 = fabsf(((float)b[i] + extSig[i] + loopOut[i]) * g3);
    if (a3 > peak3) peak3 = a3;
  }
  const float gain3 = limiterGain(limSt[3], peak3, thresh3, pole3);

  int p3 = 0;
  for (int i = 0; i < frames; i++) {
    float l3 = ((float)b[2 * i] + extSig[2 * i] + loopOut[2 * i]) * g3;
    float r3v = ((float)b[2 * i + 1] + extSig[2 * i + 1] + loopOut[2 * i + 1]) * g3;
    l3 /= gain3;
    r3v /= gain3;
    if (l3 > 32767.0f) l3 = 32767.0f;
    if (l3 < -32768.0f) l3 = -32768.0f;
    if (r3v > 32767.0f) r3v = 32767.0f;
    if (r3v < -32768.0f) r3v = -32768.0f;
    const int a = (int)fmaxf(fabsf(l3), fabsf(r3v));
    if (a > p3) p3 = a;
    b[2 * i] = (int16_t)l3;
    b[2 * i + 1] = (int16_t)r3v;
  }
  stutter::apply(b, frames, stutter::T_MAIN);

  const float f0 = p0 / 32768.0f, f1 = p1 / 32768.0f, f3 = p3 / 32768.0f;
  if (f0 > gPeak[0]) gPeak[0] = f0;
  if (f1 > gPeak[1]) gPeak[1] = f1;
  if (p2 > gPeak[2]) gPeak[2] = p2;
  if (f3 > gPeak[3]) gPeak[3] = f3;
}

int stutterDivision() { return stutHeld; }

}  // namespace ui
