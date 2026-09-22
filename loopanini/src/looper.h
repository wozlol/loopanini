#pragma once

#include <cstdint>

// Single slot looper core. Runs entirely inside the audio task (process()),
// the UI only sends commands and reads status. Loop audio lives in PSRAM.
// Records the looper input (currently the AMY synth after its fader), plays
// back into the mix, overdubs, and supports Time Machine capture from an
// always running ring while empty. CLEAR keeps the audio until new audio is
// recorded so UNDO can bring it back (single generation). Not done yet: loop
// banks (slots), pre-roll, external input, length change on the fly.
namespace looper {

enum State { EMPTY, ARMED, REC, PLAY, STOP };
enum Cmd { C_NONE, C_ARM, C_REC_NOW, C_END_REC, C_STOP, C_PLAY, C_CLEAR, C_UNDO, C_OVERDUB, C_CAPTURE };

// Written by the UI task, read by the audio task, aligned 32 bit accesses.
struct Params {
  volatile int bpm = 120;
  volatile int measures = 4;
  volatile int beats = 4;       // per measure, 4 for 4/4 and 3 for 3/4
  volatile int timeMachine = 0;
  volatile int tmGapBeats = 0;
  volatile int armNow = 0;      // ARM starts recording at once instead of on level
  volatile int autoOverdub = 0;
  volatile float threshold = 0.02f;
};
extern Params params;

// Allocates the loop buffer in PSRAM. False means no looper (no memory).
bool begin();

void command(Cmd c);          // UI task, applied at the start of the next block
State state();
bool overdubbing();
bool undoAvailable();
uint32_t rejectCount();       // bumps when a Capture is refused (silent or too short)

// Audio task. `io` is interleaved stereo looper input, left unchanged.
// `recordInput` says whether that input is armed for the looper. The loop
// playback (times returnGain) is written to `loopOut`, stereo, `frames` long,
// zero when nothing plays. Returns the peak of the playback (0 to 1).
float process(const int16_t *io, int frames, bool recordInput, float returnGain, int16_t *loopOut);

}  // namespace looper
