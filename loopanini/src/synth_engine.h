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
void setPatch(int synth, int patch, int voices);
void setLevel(int synth, float level0to1);
void setChannel(int synth, int newChannel);  // moves a synth to a different MIDI channel (to_synth)
// 1 = mono. Can be sent alone, AMY keeps the synth's current patch/sound and
// just changes the voice count, see docs/synth.md.
void setVoices(int synth, int voices);

// Loads a kit from LOOPANINI_SD_KIT_DIR (see sample_bank.h) and switches
// channel 10 to sample playback for any note the kit covers; notes the kit
// doesn't cover still hit the normal baked in drum synth. Safe to call again
// to swap kits. Returns the number of samples loaded (0 means no usable kit
// was found, channel 10 stays on the baked in kit).
int loadDrumKit(const char *dir);

// Called once per incoming 3 byte MIDI channel-voice message (note on/off)
// before it would otherwise be forwarded to AMY. Returns true if it was a
// channel 10 note on/off for a loaded sample and has been handled (the
// caller must not also forward it to AMY), false to forward as normal.
// USB device and USB host MIDI both come through as whole 3 byte messages,
// see midi_io.cpp and midi_host.cpp; DIN MIDI is a byte stream, not routed
// through this yet, see FIRMWARE_PLAN.md.
bool routeDrumNote(const uint8_t *msg3);

}  // namespace synth_engine
