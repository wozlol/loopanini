#pragma once

#include <cstdint>

// Loads 16 bit PCM WAV files from the SD card into PSRAM as AMY PCM presets
// (AMY's pcm_load(), MALLOC_CAP_SPIRAM, see synth_engine::begin). Once loaded
// a sample plays from RAM, so repeated hits never touch the SD card again,
// only the initial kit load does. See FIRMWARE_PLAN.md, Synth and sampler,
// for the folder and filename convention and how this compares to SamplerBox
// and the 1010music Blackbox.
namespace sample_bank {

// Brings up the SD card over its own SPI bus, exactly the pin lookup and
// fallback M5Unified's own Speaker_SD_wav_file example uses. Call once,
// before loadDrumKit or loadPitchedSample. False means no SD card found.
bool begin();

// Frees any samples this module has loaded (safe to call with none loaded).
void unloadAll();

// Loads every .wav directly inside `dir` (no subfolders) whose filename
// starts with a MIDI note number (e.g. "36 Kick.wav", "36_Kick.wav",
// "36.wav") as one PCM preset each, at LOOPANINI_DRUM_PRESET_BASE + note.
// Replaces any previously loaded kit. Returns the number of samples loaded,
// 0 means nothing usable was found or the SD card is not present.
int loadDrumKit(const char *dir);

// True if `note` has a sample loaded from the current kit.
bool hasDrum(int note);
// PCM preset number for `note`, only valid if hasDrum(note).
int drumPreset(int note);

// Loads a single file as the pitched sample for channel index 0..2 (our AMY
// channels 1..3), preset LOOPANINI_PITCHED_PRESET_BASE + chIndex, native
// pitch taken from the WAV's own info or defaulting to C4 (60). Replaces
// whatever was loaded for that channel index. Returns false on failure.
bool loadPitchedSample(int chIndex, const char *path);
bool hasPitched(int chIndex);
int pitchedPreset(int chIndex);

}  // namespace sample_bank
