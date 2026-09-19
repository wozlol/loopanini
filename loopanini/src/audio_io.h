#pragma once

#include <cstdint>
#include <cstddef>

// Owns ModuleAudio's I2C (ES8388 control) and I2S (the actual audio data)
// entirely. AMY never touches I2S directly on this board, see synth_engine.h,
// so this is the only place that talks to the codec.
namespace audio_io {

// Brings up the I2C link to ModuleAudio, its I2S driver at
// LOOPANINI_SAMPLE_RATE, and the ES8388 codec (mic input enabled, speaker
// output enabled, sane default levels). Assumes ModuleAudio's physical
// switch is set to B, see FIRMWARE_PLAN.md. Returns false if the module
// isn't found on the I2C bus or the codec init fails.
bool begin();

// Blocking read of one block of raw line/mic input, interleaved 16 bit
// stereo, sample_count samples per channel. Used by the looper's capture
// path once that exists. Phase 1 doesn't call this yet.
bool readBlock(int16_t *buffer, size_t sample_count);

// Blocking write of one block of interleaved 16 bit stereo audio to the
// DAC/speaker output.
bool writeBlock(const int16_t *buffer, size_t sample_count);

}  // namespace audio_io
