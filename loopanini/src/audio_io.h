#pragma once

#include <cstdint>
#include <cstddef>

// Owns every physical audio path this board can use: ModuleAudio's I2C (ES8388
// control) and I2S (the actual audio data), CoreS3's own internal speaker, and
// the class compliant USB audio interface's on/off gating. AMY never touches
// any of this directly, see synth_engine.h, so this is the only place that
// talks to audio hardware.
namespace audio_io {

// Brings up whichever analog output the persisted Audio Out preference (see
// outputPref()) selects: ModuleAudio's I2C link, I2S driver at
// LOOPANINI_SAMPLE_RATE, and the ES8388 codec (mic input enabled, speaker
// output enabled, sane default levels), or CoreS3's own internal speaker.
// Assumes ModuleAudio's physical switch is set to B when that's the selected
// path, see FIRMWARE_PLAN.md. Returns false if a selected ModuleAudio path's
// module isn't found on the I2C bus or the codec init fails; the internal
// speaker path and the no analog output path always succeed.
bool begin();

// Blocking read of one block of raw line/mic input, interleaved 16 bit
// stereo, sample_count samples per channel. Only ModuleAudio's own aux jack
// can ever be the source, there is no CoreS3 internal mic support (not
// wanted, see FIRMWARE_PLAN.md), and choosing the internal speaker or a USB
// only output means no analog input exists at all this boot: always returns
// false in both of those cases, exactly like a failed read, already handled
// gracefully by every caller (silence, not a crash or a block).
bool readBlock(int16_t *buffer, size_t sample_count);

// Blocking write of one block of interleaved 16 bit stereo audio to whichever
// analog output is active (or a no-op returning true if the preference is
// USB only, nothing to write). See setOutBufferBlocks for the optional
// queue sitting in front of the actual hardware write.
bool writeBlock(const int16_t *buffer, size_t sample_count);

// The Audio Out Config setting's raw value (ui.cpp), 0 USB, 1 Aux, 2 Both,
// 3 Int, 4 Int+USB. Persisted in its own dedicated NVS key (namespace
// "loopaudio"), not ui.cpp's own settings blob, because begin() needs it
// before ui::begin() and that blob's own load have even run. Takes effect
// on next boot only: ModuleAudio's I2S is one coupled full duplex
// peripheral with no clean teardown API, and 3 of its 5 pins (GPIO0, 13,
// 14) are physically shared with CoreS3's own internal speaker, so Aux and
// Int can never both be live, there is no safe way to switch without a
// fresh boot. USB is a fully separate subsystem (TinyUSB), no such
// restriction there, but the whole setting is kept boot time only for one
// consistent, predictable rule rather than a partly live, partly not one.
int outputPref();
void setOutputPref(int v);

// True if the ACTIVE (boot time applied, see outputPref) preference
// includes USB, i.e. v is 0, 2, or 4. loopanini.ino gates its
// usb_audio_out::writeBlock() call on this so picking Aux or Int alone
// actually turns USB off rather than leaving it silently running anyway.
bool usbAudioEnabled();

// 0-4 blocks of slack deliberately inserted between a rendered block and
// the actual hardware write: writeBlock() queues incoming blocks and only
// starts writing once this many are banked, so an occasional slow render
// (heavy polyphony, see FIRMWARE_PLAN.md) draws down the reserve instead of
// directly starving the output. Costs a fixed depth*block worth of added
// latency (0 adds none, today's exact direct-write behavior). Pure
// software queue, no peripheral to reinit, safe to change live, values
// outside 0-4 are clamped.
void setOutBufferBlocks(int n);

// Mutes whichever analog output is active and gives its DAC/amp a moment
// to settle. Call immediately before esp_restart(): a software reset does
// not wind peripherals down first, so whatever was last playing cuts off
// abruptly, heard as a loud pop, common on audio hardware generally, not
// specific to this board, muting first is the standard fix.
void muteBeforeReboot();

}  // namespace audio_io
