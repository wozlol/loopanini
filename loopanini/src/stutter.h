#pragma once

#include <cstdint>

// Audio stutter, modeled on arpnmidi's Stutter. A rolling history of the
// target audio is always recording. While a division is held, the most recent
// slice of that length is frozen and repeated in place of the target audio
// (the underlying audio is suppressed), and letting go resumes normal audio.
// Slice length = the division at the current looper BPM. Repeats are never
// recorded into the looper because the looper records its input before this.
namespace stutter {

enum Target { T_LOOPER = 0, T_SYNTH = 1, T_MAIN = 2, T_AUX = 3 };

// PSRAM history, false if it could not be allocated (stutter then does nothing).
bool begin();

// UI task. division is the grid index 0..11 or -1 for released, target is a Target.
void set(int division, int target);

// Audio task. Call once per block on each of the four streams, it acts only on
// the stream that matches the selected target.
void apply(int16_t *io, int frames, Target which);

}  // namespace stutter
