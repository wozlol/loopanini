#include "usb_audio_out.h"

#include <algorithm>

#include "USBAudioCard.h"

#include "config.h"

namespace usb_audio_out {

namespace {
// UAC_SPK_NONE: no host-to-device path at all, so there's no playback
// clock or buffer to manage, only the mic (device-to-host) side exists.
USBAudioCard uac(LOOPANINI_SAMPLE_RATE, UAC_BPS_16, UAC_SPK_NONE, UAC_MIC_STEREO);

float level = LOOPANINI_USB_AUDIO_OUT_LEVEL_DEFAULT;

// Namespace-scope, not on the audio task's stack: sized for AMY's largest
// documented block (BLOCK_SIZE_BITS up to 10, i.e. 1024 samples/channel),
// stereo. Actual AMY_BLOCK_SIZE today is far smaller (128), this just
// avoids a stack-sized buffer in a task whose stack is otherwise sized for
// the render/MIDI/I2S work, not a second scratch block.
constexpr size_t kMaxSamplesPerChannel = 1024;
int16_t scratch[kMaxSamplesPerChannel * 2];
}  // namespace

void begin() { uac.begin(); }

void setLevel(float new_level) { level = new_level; }

void writeBlock(const int16_t *buffer, size_t sample_count) {
  size_t total = std::min(sample_count, kMaxSamplesPerChannel) * 2;
  for (size_t i = 0; i < total; ++i) {
    scratch[i] = static_cast<int16_t>(buffer[i] * level);
  }
  uac.write(scratch, static_cast<uint16_t>(total * sizeof(int16_t)));
}

}  // namespace usb_audio_out
