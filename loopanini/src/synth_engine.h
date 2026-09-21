#pragma once

#include <cstdint>

// Thin wrapper around AMY running headless: no I2S, no built-in MIDI
// transport. ModuleAudio owns the actual audio hardware (audio_io.h) and our
// own MIDI transports (midi_io.h) own USB and DIN, feeding AMY's parser
// directly. This keeps AMY out of any transport code whose Arduino support
// assumes official AMY board hardware we don't have, see FIRMWARE_PLAN.md's
// Toolchain section.
namespace synth_engine {

// Starts AMY with the default synths on MIDI channels 1, 2, and 10.
void begin();

// Renders one AMY_BLOCK_SIZE block and returns a pointer to it: interleaved
// 16 bit stereo, owned by AMY, valid until the next call. Never call this
// from more than one task.
int16_t *renderBlock();

// Live parameter changes from the UI. Safe from any task (amy_add_event queues).
void setPatch(int synth, int patch);
void setLevel(int synth, float level0to1);

}  // namespace synth_engine
