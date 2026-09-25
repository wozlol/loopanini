#include "synth_engine.h"

#include <AMY-Arduino.h>

#include "config.h"
#include "debug_io.h"
#include "sample_bank.h"

namespace synth_engine {
namespace {

// Notes currently sounding through the sample layer, so a note-off knows
// whether it was ours to handle. -1 means not one of ours.
int8_t drumOsc[128];
uint16_t nextDrumOsc = 0;

extern "C" int16_t *pcm_load(uint16_t, uint32_t, uint32_t, uint8_t, float, uint32_t, uint32_t);

// Fires from inside amy_overload_check() (called every block from
// loopanini.ino's audioTask, see its comment) when sustained render load
// trips AMY's own failsafe: it resets all notes/oscs and plays a
// descending "doot doot doot doot" so this is audible, not just logged,
// separately from whatever else is causing dropouts. AMY's own message for
// this goes to plain stderr, which on this board is not the USB CDC port
// debug_io.h logs to, so without this hook that message is invisible in
// the Serial Monitor. Load the earlier once a second report already print
// with debug_io.
void onOverload(float load) {
  debug_io::out().printf("AMY: overload failsafe fired, render load was %.0f%%, resetting\n",
                          load * 100.0f);
}

}  // namespace

void begin() {
  amy_config_t amy_config = amy_default_config();
  amy_config.features.startup_bleep = 1;
  amy_config.features.default_synths = 1;  // synths on MIDI ch 1, 2, 10
  amy_config.amy_external_overload_hook = onOverload;

  // ModuleAudio (audio_io.h) owns I2S. Our own MIDI transports (midi_io.h)
  // own USB and DIN and feed AMY's parser directly, so AMY does neither
  // here.
  amy_config.audio = AMY_AUDIO_IS_NONE;
  amy_config.midi = AMY_MIDI_IS_NONE;

  // SD sample caching (sample_bank.cpp) calls AMY's pcm_load() directly and
  // needs its sample RAM in PSRAM, not the small internal heap. Left at the
  // non-Tulip/AMYboard default (MALLOC_CAP_DEFAULT, internal RAM) otherwise.
  amy_config.ram_caps_sample = MALLOC_CAP_SPIRAM;

  amy_start(amy_config);

  for (int i = 0; i < 128; i++) drumOsc[i] = -1;
}

int16_t *renderBlock() {
  int16_t *buf = amy_simple_fill_buffer();
  // amy_fill_buffer() (amy.c) halves every sample right after its own soft
  // clipper on any ESP_PLATFORM build: "For some reason, have to drop a bit
  // to stop hard wrapping on esp?" (their comment, still an open question as
  // of shorepine/amy#1169, filed by AMY's own maintainer). The soft clip
  // already bounds the value to SAMPLE_MAX=32767 before that shift runs, so
  // every ESP32 build has shipped at a hard -6dBFS ceiling, one bit of DAC
  // resolution thrown away, independent of any patch/velocity/volume
  // setting. #1169's own PR only excludes ESP32-P4, Xtensa ESP32/S3 (this
  // board) is explicitly still affected. Undoing it here rather than
  // patching the vendored library: a hand edit to a globally installed
  // Arduino library is fragile (silently lost on the next library update,
  // and would affect every other sketch that uses it), this is a 1 line,
  // self contained fix scoped to this project. Double back up with a
  // defensive clamp, 1 LSB of rounding loss from the original >>=1 is
  // inaudible.
  for (int i = 0; i < AMY_BLOCK_SIZE * 2; i++) {
    int32_t v = (int32_t)buf[i] * 2;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    buf[i] = (int16_t)v;
  }
  return buf;
}

void setPatch(int synth, int patch, int voices) {
  amy_event e = amy_default_event();
  e.synth = (uint8_t)synth;
  e.patch_number = (uint16_t)patch;
  e.num_voices = (uint8_t)voices;
  amy_add_event(&e);
}

void setVoices(int synth, int voices) {
  amy_event e = amy_default_event();
  e.synth = (uint8_t)synth;
  e.num_voices = (uint8_t)voices;
  amy_add_event(&e);
}

void setLevel(int synth, float level) {
  amy_event e = amy_default_event();
  e.synth = (uint8_t)synth;
  e.synth_level = level;
  amy_add_event(&e);
}

void setChannel(int synth, int newChannel) {
  amy_event e = amy_default_event();
  e.synth = (uint8_t)synth;
  e.to_synth = (uint8_t)newChannel;
  amy_add_event(&e);
}

int loadDrumKit(const char *dir) { return sample_bank::loadDrumKit(dir); }

bool routeDrumNote(const uint8_t *msg3) {
  const uint8_t status = msg3[0] & 0xF0;
  const uint8_t channel = msg3[0] & 0x0F;
  if (channel != 9) return false;  // MIDI channel 10 is nibble 9
  if (status != 0x90 && status != 0x80) return false;
  const uint8_t note = msg3[1] & 0x7F;
  const uint8_t vel = msg3[2] & 0x7F;
  const bool noteOn = status == 0x90 && vel > 0;

  if (!sample_bank::hasDrum(note)) return false;  // not ours, fall through to AMY

  if (noteOn) {
    const uint16_t osc = LOOPANINI_DRUM_OSC_BASE + nextDrumOsc;
    nextDrumOsc = (nextDrumOsc + 1) % LOOPANINI_DRUM_OSC_COUNT;
    drumOsc[note] = (int8_t)nextDrumOsc;  // remembers the slot, not strictly needed for one-shots
    amy_event e = amy_default_event();
    e.osc = osc;
    e.wave = PCM;  // amy.h: #define PCM 7
    e.preset = (int16_t)sample_bank::drumPreset(note);
    e.velocity = vel / 127.0f;
    e.midi_note = (float)note;
    amy_add_event(&e);
  }
  // Drum hits are one-shots (PCM_PLAY_STOP is AMY's default PCM mode), no
  // explicit note-off handling needed, so a note-off is simply swallowed.
  return true;
}

}  // namespace synth_engine
