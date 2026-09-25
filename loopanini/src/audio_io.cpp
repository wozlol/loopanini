#include "audio_io.h"

#include "M5Unified.h"
#include "M5Module_Audio.h"

#include "config.h"
#include "pin_guard.h"

namespace audio_io {

namespace {
M5ModuleAudio device;
}  // namespace

bool begin() {
  // ModuleAudio's I2S pins on CoreS3, switch B. This mirrors
  // M5ModuleAudio::begin(m5::I2C_Class&, addr, speed)'s own automatic
  // mapping (see Module-Audio's i2sloopback example), except we pass
  // LOOPANINI_SAMPLE_RATE explicitly instead of that overload's hardcoded
  // 44100, so ModuleAudio's output matches AMY's fixed 48 kHz render rate
  // with no resampling needed anywhere in between.
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  const uint8_t bck = M5.getPin(m5::pin_name_t::mbus_pin24);  // SCLK
  const uint8_t mck = M5.getPin(m5::pin_name_t::mbus_pin22);  // MCLK
#else
  const uint8_t bck = M5.getPin(m5::pin_name_t::mbus_pin22);  // SCLK
  const uint8_t mck = M5.getPin(m5::pin_name_t::mbus_pin24);  // MCLK
#endif
  const uint8_t di = M5.getPin(m5::pin_name_t::mbus_pin26);
  const uint8_t ws = M5.getPin(m5::pin_name_t::mbus_pin21);  // LRCK
  const uint8_t do_ = M5.getPin(m5::pin_name_t::mbus_pin23);

  // Audio claims its pins first and wins every conflict, see pin_guard.h
  // and the pin budget in config.h. On CoreS3 with switch B these resolve to
  // LRCK GPIO6, MCLK GPIO7, DOUT GPIO13, BCK GPIO0, DIN GPIO14.
  const pin_guard::PinClaim pins[] = {
      {ws, "ModuleAudio I2S LRCK"}, {mck, "ModuleAudio I2S MCLK"}, {do_, "ModuleAudio I2S DOUT"},
      {bck, "ModuleAudio I2S BCK"}, {di, "ModuleAudio I2S DIN"},
  };
  if (!pin_guard::claimAll(pins, sizeof(pins) / sizeof(pins[0]))) return false;

  if (!device.begin(M5.In_I2C, LOOPANINI_MODULEAUDIO_I2C_ADDR,
                     LOOPANINI_MODULEAUDIO_I2C_SPEED, LOOPANINI_SAMPLE_RATE,
                     mck, di, ws, do_, bck)) {
    return false;
  }

  // begin()'s sample_rate argument only configures the ESP32 side of the
  // I2S bus. The ES8388 codec's own ADC/DAC clock dividers are a separate
  // set of registers that es8388->init() (inside begin()) never touches,
  // so without this call the codec runs its internal conversion clock at
  // whatever its own reset default is, not LOOPANINI_SAMPLE_RATE, mismatched
  // against the I2S bus timing we just set up. M5Stack's own i2sloopback
  // example calls this too, right after begin(), for the same reason.
  if (!device.setSampleRate(SAMPLE_RATE_48K)) {
    return false;
  }

  // Phase 1 defaults: Ext input (ModuleAudio's own mic/line jack) into the
  // codec's line input, speaker/headphone output live. The Ext/Int input
  // toggle and the CoreS3-speaker output toggle from FIRMWARE_PLAN.md come
  // later, once there's a settings screen to put them on.
#if LOOPANINI_AUX_ADC_INPUT == 1
  device.setMicInputLine(ADC_INPUT_LINPUT1_RINPUT1);
#else
  device.setMicInputLine(ADC_INPUT_LINPUT2_RINPUT2);
#endif
  // es_mic_gain_t's values are 0=0dB, 1=3dB, ... 8=24dB, i.e. the enum index
  // is the gain in dB divided by 3, see es8388.hpp. LOOPANINI_AUX_MIC_GAIN_DB
  // is asserted to one of that fixed set below so an out of range value
  // fails to compile instead of silently picking the nearest one.
  static_assert(LOOPANINI_AUX_MIC_GAIN_DB % 3 == 0 && LOOPANINI_AUX_MIC_GAIN_DB >= 0 &&
                    LOOPANINI_AUX_MIC_GAIN_DB <= 24,
                "LOOPANINI_AUX_MIC_GAIN_DB must be 0, 3, 6, ... 24");
  device.setMicGain((es_mic_gain_t)(LOOPANINI_AUX_MIC_GAIN_DB / 3));
  device.setMicAdcVolume(80);
  // es8388->init() (inside begin(), above) sets the analog Lout/Rout volume
  // registers to a conservative "-45dB" power-up default and never raises
  // it again, M5Stack's own reference example doesn't either. Max volume
  // here for bring-up, both output channels enabled rather than just one
  // so this doesn't depend on knowing which physical path ModuleAudio's
  // jack is actually wired to, and an explicit unmute so that isn't left
  // to chance either. Dial back down once the Mixer screen exists.
  device.setSpeakerVolume(100);
  device.setSpeakerOutput(DAC_OUTPUT_ALL);
  device.setMute(false);
  device.setHPMode(AUDIO_HPMODE_NATIONAL);
  device.setMICStatus(AUDIO_MIC_OPEN);
  device.setBitsSample(ES_MODULE_ADC_DAC, BIT_LENGTH_16BITS);

  return true;
}

bool readBlock(int16_t *buffer, size_t sample_count) {
  // Stereo interleaved, so a block of sample_count samples per channel is
  // sample_count * 2 int16_t's.
  const bool ok = device.record(reinterpret_cast<uint8_t *>(buffer),
                                 static_cast<int>(sample_count * 2 * sizeof(int16_t)));
  if (ok) {
    // ModuleAudio's inputs are mono mic jacks, not a stereo pair: M5Stack's
    // own product spec lists "2-channel mic input" (two independent mono mic
    // paths, one per 3.5mm jack), and the driver docs confirm both jacks
    // share the ES8388's single LIN1 pin. RIN1 is unwired on this input, so
    // its captured samples are floating-pin noise, not signal, that's the
    // crackle a real stereo source shows on the right channel. Fold left
    // into right here, once, so every consumer downstream (mixer, looper,
    // stutter, meters) sees clean mono-as-stereo instead of noise.
    for (size_t i = 0; i < sample_count; i++) buffer[2 * i + 1] = buffer[2 * i];
  }
  return ok;
}

bool writeBlock(const int16_t *buffer, size_t sample_count) {
  return device.play(reinterpret_cast<const uint8_t *>(buffer),
                      static_cast<int>(sample_count * 2 * sizeof(int16_t)));
}

}  // namespace audio_io
