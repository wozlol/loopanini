#pragma once

#include <cstdint>

// Touch UI for the 320x240 display plus the 40 px touch strip below it
// (touch area 320x280). Five screens: Mixer, AMY synth, Looper, Stutter,
// Config, changed by tapping or swiping in the strip. See FIRMWARE_PLAN.md,
// Touch UI. Looper, limiter, compressor and AMY parameters are UI state only
// for now, the mixer level and mute for the synth and main channels are live.
namespace ui {

// Call once from setup() after M5.begin() and spi_lock::init().
void begin();

// Call from loop() on core 1, every few milliseconds. Calls M5.update().
void update();

// Called by the audio task on every block: applies the synth and main
// faders and mutes to the interleaved stereo block and feeds the meters.
void processBlock(int16_t *interleavedStereo, int frames);

// Stutter division currently held (index into the grid) or -1.
int stutterDivision();

}  // namespace ui
