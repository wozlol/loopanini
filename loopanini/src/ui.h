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

// Called by the audio task on every block. `synthBlock` is AMY's rendered
// output (interleaved stereo), overwritten in place with the final mixed
// output (INT/EXT/LOP faders and mutes applied, looper mixed in, stutter
// applied) ready for audio_io::writeBlock and usb_audio_out. `auxBlock` is
// this block's live input from audio_io::readBlock (ModuleAudio's mic/line
// jack), read but not modified.
void processBlock(int16_t *synthBlock, const int16_t *auxBlock, int frames);

// Stutter division currently held (index into the grid) or -1.
int stutterDivision();

}  // namespace ui
