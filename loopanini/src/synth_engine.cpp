#include "synth_engine.h"

#include <AMY-Arduino.h>

namespace synth_engine {

void begin() {
  amy_config_t amy_config = amy_default_config();
  amy_config.features.startup_bleep = 1;
  amy_config.features.default_synths = 1;  // synths on MIDI ch 1, 2, 10

  // ModuleAudio (audio_io.h) owns I2S. Our own MIDI transports (midi_io.h)
  // own USB and DIN and feed AMY's parser directly, so AMY does neither
  // here.
  amy_config.audio = AMY_AUDIO_IS_NONE;
  amy_config.midi = AMY_MIDI_IS_NONE;

  amy_start(amy_config);
}

int16_t *renderBlock() { return amy_simple_fill_buffer(); }

}  // namespace synth_engine
