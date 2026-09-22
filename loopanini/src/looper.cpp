#include "looper.h"

#include <cmath>
#include <cstdlib>

#include <esp_heap_caps.h>

#include "config.h"

namespace looper {

Params params;

namespace {

int16_t *buf = nullptr;
uint32_t cap = 0;  // frames

volatile State st = EMPTY;
volatile bool od = false;
volatile bool undoable = false;
volatile Cmd pending = C_NONE;
volatile uint32_t rejects = 0;

uint32_t loopStart = 0, loopLen = 0, pos = 0, recPos = 0;
uint32_t ringPos = 0, ringFilled = 0, sinceLoud = 0xFFFFFFFFu;

uint32_t beatFrames() {
  int bpm = params.bpm;
  if (bpm < 20) bpm = 20;
  return (uint32_t)(60.0 * LOOPANINI_SAMPLE_RATE / bpm);
}

uint32_t wantedLen() {
  uint32_t l = beatFrames() * (uint32_t)(params.measures * params.beats);
  if (l > cap) l = cap;
  return l;
}

void beginRecord() {
  loopStart = 0;
  recPos = 0;
  loopLen = wantedLen();
  undoable = false;
  st = REC;
}

void finishRecord() {
  pos = 0;
  od = params.autoOverdub != 0;
  st = PLAY;
}

void service() {
  const Cmd c = pending;
  if (c == C_NONE) return;
  pending = C_NONE;
  if (!buf) return;
  switch (c) {
    case C_ARM:
      if (st == EMPTY && !params.timeMachine) {
        if (params.armNow)
          beginRecord();
        else
          st = ARMED;
      }
      break;
    case C_REC_NOW:
      if (st == ARMED) beginRecord();
      break;
    case C_END_REC:
      if (st == REC && recPos > 0) {
        loopLen = recPos;
        finishRecord();
      }
      break;
    case C_STOP:
      if (st == PLAY) {
        st = STOP;
        od = false;
      } else if (st == REC || st == ARMED) {
        st = EMPTY;
        undoable = false;
      }
      break;
    case C_PLAY:
      if (st == STOP) {
        pos = 0;
        st = PLAY;
      }
      break;
    case C_CLEAR:
      if (st == STOP) {
        st = EMPTY;
        od = false;
        undoable = loopLen > 0;
        ringFilled = 0;
        ringPos = 0;
        sinceLoud = 0xFFFFFFFFu;
      }
      break;
    case C_UNDO:
      if (st == EMPTY && undoable) {
        pos = 0;
        undoable = false;
        st = STOP;
      }
      break;
    case C_OVERDUB:
      if (st == PLAY) od = !od;
      break;
    case C_CAPTURE:
      if (st == EMPTY && params.timeMachine) {
        const uint32_t len = wantedLen();
        uint32_t gap = beatFrames() * (uint32_t)params.tmGapBeats;
        const uint32_t need = len + gap;
        if (len == 0 || need > cap || ringFilled < need || sinceLoud > need) {
          rejects = rejects + 1;
        } else {
          loopLen = len;
          loopStart = (ringPos + cap - need) % cap;
          pos = gap % len;
          undoable = false;
          od = params.autoOverdub != 0;
          st = PLAY;
        }
      }
      break;
    default:
      break;
  }
}

inline int16_t clip16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

}  // namespace

bool begin() {
  uint32_t frames = (uint32_t)LOOPANINI_LOOPER_MAX_SECONDS * LOOPANINI_SAMPLE_RATE;
  int16_t *mem = nullptr;
  while (frames >= LOOPANINI_SAMPLE_RATE * 2u) {
    mem = (int16_t *)heap_caps_malloc((size_t)frames * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mem) break;
    frames /= 2;
  }
  if (!mem) return false;
  params.threshold = LOOPANINI_LOOPER_ARM_THRESHOLD;
  cap = frames;
  buf = mem;  // last, the audio task treats a non null buf as ready
  return true;
}

void command(Cmd c) { pending = c; }
State state() { return st; }
bool overdubbing() { return od; }
bool undoAvailable() { return undoable; }
uint32_t rejectCount() { return rejects; }

float process(const int16_t *io, int frames, bool recordInput, float returnGain, int16_t *loopOut) {
  for (int i = 0; i < frames * 2; i++) loopOut[i] = 0;
  service();
  if (!buf) return 0.0f;

  int pk = 0;
  if (recordInput) {
    for (int i = 0; i < frames * 2; i++) {
      const int a = abs((int)io[i]);
      if (a > pk) pk = a;
    }
  }
  const bool loud = pk >= (int)(params.threshold * 32767.0f);
  if (loud)
    sinceLoud = 0;
  else if (sinceLoud < 0xFFFFFFF0u)
    sinceLoud += frames;
  if (st == ARMED && loud) beginRecord();

  int playPeak = 0;
  for (int f = 0; f < frames; f++) {
    const int32_t inL = recordInput ? io[2 * f] : 0;
    const int32_t inR = recordInput ? io[2 * f + 1] : 0;
    switch (st) {
      case EMPTY:
        if (params.timeMachine) {
          undoable = false;
          buf[ringPos * 2] = (int16_t)inL;
          buf[ringPos * 2 + 1] = (int16_t)inR;
          if (++ringPos >= cap) ringPos = 0;
          if (ringFilled < cap) ringFilled++;
        }
        break;
      case REC:
        buf[recPos * 2] = (int16_t)inL;
        buf[recPos * 2 + 1] = (int16_t)inR;
        if (++recPos >= loopLen) finishRecord();
        break;
      case PLAY: {
        uint32_t idx = loopStart + pos;
        if (idx >= cap) idx -= cap;
        int16_t *p = &buf[idx * 2];
        const int32_t oL = (int32_t)(p[0] * returnGain);
        const int32_t oR = (int32_t)(p[1] * returnGain);
        const int a = abs(oL) > abs(oR) ? abs(oL) : abs(oR);
        if (a > playPeak) playPeak = a;
        if (od && recordInput) {
          p[0] = clip16((int32_t)p[0] + inL);
          p[1] = clip16((int32_t)p[1] + inR);
        }
        loopOut[2 * f] = clip16(oL);
        loopOut[2 * f + 1] = clip16(oR);
        if (++pos >= loopLen) pos = 0;
        break;
      }
      default:
        break;
    }
  }
  return playPeak / 32768.0f;
}

}  // namespace looper
