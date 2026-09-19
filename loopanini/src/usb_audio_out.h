#pragma once

#include <cstddef>
#include <cstdint>

// CoreS3's native USB port as a class-compliant, output-only (device to
// host) 2 channel USB audio interface: the computer sees loopanini as a
// stereo capture source, no USB playback path exists (UAC_SPK_NONE), so
// there is no USB audio clock coming back the other way to sync against.
// Composes with midi_io.h's USBMIDI on the same physical port as one
// composite device, both classes register themselves and a single shared
// USB.begin() in loopanini.ino brings the whole composite device up. See
// FIRMWARE_PLAN.md's Audio path and Mixer sections.
namespace usb_audio_out {

// Registers the UAC interface. Call before the shared USB.begin() in
// loopanini.ino, not after.
void begin();

// Sets the USB output level, independent of ModuleAudio/CoreS3 speaker's
// analog output level, see FIRMWARE_PLAN.md's Mixer section. 0.0 is
// silence, 1.0 is unity.
void setLevel(float level);

// Sends one block of interleaved 16 bit stereo audio to the host, scaled by
// the level set with setLevel(). Called once per rendered block from the
// audio task, same cadence as audio_io::writeBlock().
void writeBlock(const int16_t *buffer, size_t sample_count);

}  // namespace usb_audio_out
