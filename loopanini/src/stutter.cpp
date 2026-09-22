#include "stutter.h"

#include <esp_heap_caps.h>

#include "config.h"
#include "looper.h"

namespace stutter {
namespace {

constexpr uint32_t kMaxSlice = 4u * LOOPANINI_SAMPLE_RATE;  // 4 s of stereo history
constexpr uint32_t kMinSlice = 64;

// Length in beats for each grid index, same order as the UI grid.
const float kBeats[12] = {4.0f,  2.0f,  1.0f,  0.5f,   0.25f,  0.125f,
                          4.0f / 3, 2.0f / 3, 1.0f / 3, 1.0f / 6, 1.5f, 0.75f};

int16_t *ring = nullptr;
uint32_t wpos = 0, filled = 0;

volatile int wantDiv = -1;
volatile int wantTarget = T_MAIN;

bool active = false;
int activeDiv = -1;
int activeTarget = -1;
uint32_t sliceStart = 0, sliceLen = 0, rpos = 0;

uint32_t sliceFrames(int div) {
  int bpm = looper::params.bpm;
  if (bpm < 20) bpm = 20;
  double f = kBeats[div] * 60.0 * LOOPANINI_SAMPLE_RATE / bpm;
  if (f > kMaxSlice) f = kMaxSlice;
  return (uint32_t)f;
}

}  // namespace

bool begin() {
  int16_t *mem = (int16_t *)heap_caps_malloc((size_t)kMaxSlice * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!mem) return false;
  ring = mem;
  return true;
}

void set(int division, int target) {
  wantTarget = target;
  wantDiv = division;
}

void apply(int16_t *io, int frames, Target which) {
  if (!ring) return;
  const int target = wantTarget;
  if (target != which) {
    if (activeTarget == (int)which) active = false;  // deselected mid hold
    return;
  }
  if (activeTarget != target) {  // target changed, old history is another stream
    activeTarget = target;
    active = false;
    filled = 0;
    wpos = 0;
  }
  const int div = wantDiv;
  if (div < 0) {
    active = false;
  } else if (!active || div != activeDiv) {
    uint32_t len = sliceFrames(div);
    if (len > filled) len = filled;
    if (len >= kMinSlice) {
      sliceLen = len;
      sliceStart = (wpos + kMaxSlice - len) % kMaxSlice;
      rpos = 0;
      activeDiv = div;
      active = true;
    }
  }

  for (int f = 0; f < frames; f++) {
    if (active) {
      uint32_t idx = sliceStart + rpos;
      if (idx >= kMaxSlice) idx -= kMaxSlice;
      io[2 * f] = ring[idx * 2];
      io[2 * f + 1] = ring[idx * 2 + 1];
      if (++rpos >= sliceLen) rpos = 0;
    } else {
      ring[wpos * 2] = io[2 * f];
      ring[wpos * 2 + 1] = io[2 * f + 1];
      if (++wpos >= kMaxSlice) wpos = 0;
      if (filled < kMaxSlice) filled++;
    }
  }
}

}  // namespace stutter
