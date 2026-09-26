#include "audio_io.h"

#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <Preferences.h>

#include "M5Unified.h"
#include "M5Module_Audio.h"

#include "config.h"
#include "pin_guard.h"

namespace audio_io {

namespace {
M5ModuleAudio device;

enum AnalogOut { ANALOG_NONE, ANALOG_MODULE, ANALOG_INTERNAL };
// Both decided once, in begin(), from the persisted Audio Out preference,
// see outputPref()'s own comment on why this is boot time only.
AnalogOut activeAnalog = ANALOG_MODULE;
bool activeUsb = true;

constexpr const char *kPrefNamespace = "loopaudio";
constexpr const char *kPrefKey = "outpref";

int readPref() {
  Preferences p;
  p.begin(kPrefNamespace, /*readOnly=*/true);
  // Default 2 ("Both"): Aux and USB both always ran unconditionally before
  // this setting existed, a board with nothing saved yet (or an older
  // build's NVS) boots into that same familiar behavior, not a surprise.
  const int v = p.getInt(kPrefKey, 2);
  p.end();
  return (v >= 0 && v <= 4) ? v : 2;
}

// ---- CoreS3's own internal speaker path ----
// M5.Speaker.playRaw() queues a request to its own background task and
// returns immediately, unlike ModuleAudio's device.play() which only
// returns once the data is actually copied out. Reusing a buffer before
// that background task is actually done reading it would corrupt what
// plays, so buffers are only reused once the task's own release callback
// confirms it is done with that specific pointer. See FIRMWARE_PLAN.md
// for why this exists and ModuleAudio's own path (writeToActiveBackend,
// ANALOG_MODULE) does not need it: device.play() is a genuinely blocking
// call, safe to reuse its buffer the moment it returns, no such race
// there.
//
// First hardware test came back badly glitched on this path specifically
// (described as "ring mod", tiny bits missing), traced to
// Speaker_Class's own default config: task_priority 2, well below this
// project's audio task (5), and task_pinned_core ~0, meaning M5Unified
// lets FreeRTOS pick either core rather than pinning it, so it could
// easily land on the SAME core as the audio task (LOOPANINI_AUDIO_TASK_CORE)
// and get starved by it, exactly the kind of same-core contention this
// project pins the audio task away from everywhere else. Pinned to the
// other core explicitly in beginInternalSpeaker below, and bumped from 2
// buffers to 3 for a little extra margin against whatever scheduling
// jitter is left on its own core.
constexpr int kSpkBufN = 3;
int16_t *spkBuf[kSpkBufN] = {};
size_t spkBufLen = 0;
int spkNext = 0;
volatile bool spkFree[kSpkBufN] = {true, true, true};
// Counting semaphore, given once per release callback, taken by
// writeInternal's wait. vTaskDelay(1) polling (previous pass) fixed the
// crash (it yields, unlike delayMicroseconds) but still garbled both
// Int and, through it, USB too (confirmed on hardware: Ext+USB stayed
// clean, only modes with Int active glitched, so this task's own
// cadence, not USB, is the shared cause). A poll tick (~1ms on this
// project) versus a ~2.7ms block period is coarse enough to drift: most
// iterations need at least one tick's wait, and a wait that is
// systematically a little more or a little less than the real block
// period accumulates into exactly this kind of periodic artifact over
// many blocks. A semaphore wakes the instant a slot actually frees,
// whichever tick that falls on, no polling granularity to drift against,
// which is what the library's own docs mean by "filled only after the
// previous one is released" as the real time reference, not a timer.
SemaphoreHandle_t spkSem = nullptr;

void onSpkBufferReleased(void *, const void *data, uint8_t) {
  for (int i = 0; i < kSpkBufN; i++)
    if (data == spkBuf[i]) spkFree[i] = true;
  if (spkSem) xSemaphoreGive(spkSem);
}

bool beginInternalSpeaker() {
  // M5.begin() (loopanini.ino's setup(), always runs before audio_io::begin())
  // already populated M5.Speaker's own config with CoreS3's real pins,
  // internal_spk defaults true in M5Unified's own Config. Reading them
  // back here rather than hardcoding stays correct if that default ever
  // changes upstream. Not const: task_pinned_core is overridden below
  // before this modified copy is written back and begin() actually
  // starts the background task.
  auto cfg = M5.Speaker.config();
  const pin_guard::PinClaim pins[] = {
      {cfg.pin_bck, "CoreS3 internal speaker I2S BCK"},
      {cfg.pin_ws, "CoreS3 internal speaker I2S WS"},
      {cfg.pin_data_out, "CoreS3 internal speaker I2S DATA_OUT"},
  };
  if (!pin_guard::claimAll(pins, sizeof(pins) / sizeof(pins[0]))) return false;
  cfg.task_pinned_core = (LOOPANINI_AUDIO_TASK_CORE == 0) ? 1 : 0;
  M5.Speaker.config(cfg);
  if (spkSem == nullptr) spkSem = xSemaphoreCreateCounting(kSpkBufN, 0);
  M5.Speaker.setBufferReleaseCallback(nullptr, onSpkBufferReleased);
  if (!M5.Speaker.begin()) return false;
  // Was 255 (max), a real hardware bug, not just loud: reported as a loud
  // "burp" then a genuine ESP_RST_POWERON reboot (not brownout, not panic,
  // resetReasonName distinguishes them, this project's own boot log
  // confirmed "power on" specifically), a true power-on reset only fires
  // from a much harder voltage collapse than the brownout detector's own
  // threshold, consistent with this small onboard amp's own current draw
  // spiking hard enough at full volume, worse combined with USB's own
  // draw, to genuinely collapse the rail rather than just dip it. Likely
  // made worse, not caused, by the previous pass's pacing fix: choppy,
  // frequently dropped audio has a lower average duty cycle than smooth,
  // continuous audio at the same peak volume, so fixing the audio quality
  // bug plausibly removed an accidental, lower average power draw that
  // had been quietly staying under the brownout line. 80 is a
  // conservative, still clearly audible default, well under max, safer
  // for a small onboard amp sharing power with everything else on this
  // board, dial up from Config once there is a volume setting for this
  // path specifically, do not just bump this constant back toward 255.
  M5.Speaker.setVolume(80);
  return true;
}

bool writeInternal(const int16_t *buffer, size_t sample_count) {
  const size_t need = sample_count * 2;  // stereo interleaved
  if (spkBufLen != need) {
    for (auto &b : spkBuf) {
      delete[] b;
      b = new int16_t[need];
    }
    spkBufLen = need;
    for (auto &f : spkFree) f = true;
  }
  const int i = spkNext;
  // Found the real bug behind "more consistently ring mod" on hardware:
  // ModuleAudio's device.play() blocks until the I2S hardware actually
  // wants more data, which is the ONLY thing that paces this whole audio
  // task to real time (confirmed against loopanini.ino's audioTask, which
  // does not otherwise wait anywhere for a successful write). playRaw()
  // never blocks, so with ANALOG_INTERNAL active nothing paces this task
  // at all, it renders and calls this every ~2.7ms (AMY_BLOCK_SIZE at
  // LOOPANINI_SAMPLE_RATE) as fast as render/mix cost allows, typically
  // under 1ms per the render_us this project already logs, meaning it
  // was racing 2-3x realtime, guaranteed to outrun a 48kHz consumer no
  // matter how many buffers or which core its background task runs on.
  // Task pinning (twenty ninth pass) was a real fix for a real problem
  // (same core starvation) but never the whole story, this task racing
  // ahead unpaced would still overrun a perfectly scheduled consumer.
  // Waiting here for an actual free slot, bounded so a genuinely stuck
  // background task can never hang the audio task indefinitely, restores
  // the same real time pacing device.play()'s own blocking already gives
  // the ModuleAudio path for free.
  // Two earlier attempts at this exact wait each fixed one problem and
  // left another: delayMicroseconds() (thirty first pass) never yields,
  // starved core 0's idle task, a real boot loop on real hardware.
  // vTaskDelay(1) polling (thirty third pass) yields, fixing that crash,
  // but a ~1ms poll tick against a ~2.7ms block period is coarse enough
  // to drift, and it did, confirmed on hardware still garbled with no
  // crash. A semaphore given by the release callback wakes this the
  // instant a slot actually frees, no polling interval to drift against.
  // One catch: a give is not necessarily for THIS slot specifically (3
  // buffers rotate), so re-check spkFree[i] after every take rather than
  // assuming one give means this exact index is ready, bounded by an
  // absolute deadline so a genuinely stuck background task still cannot
  // hang this indefinitely.
  if (!spkFree[i] && spkSem != nullptr) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(4);
    while (!spkFree[i]) {
      const TickType_t now = xTaskGetTickCount();
      if (now >= deadline) break;
      xSemaphoreTake(spkSem, deadline - now);
    }
  }
  if (!spkFree[i]) return false;  // background task truly stuck, drop rather than corrupt it
  memcpy(spkBuf[i], buffer, need * sizeof(int16_t));
  spkFree[i] = false;
  spkNext = (spkNext + 1) % kSpkBufN;
  return M5.Speaker.playRaw(spkBuf[i], need, LOOPANINI_SAMPLE_RATE, /*stereo=*/true,
                            /*repeat=*/1, /*channel=*/-1, /*stop_current_sound=*/false);
}

bool writeToActiveBackend(const int16_t *buffer, size_t sample_count) {
  switch (activeAnalog) {
    case ANALOG_MODULE:
      return device.play(reinterpret_cast<const uint8_t *>(buffer),
                          static_cast<int>(sample_count * 2 * sizeof(int16_t)));
    case ANALOG_INTERNAL:
      return writeInternal(buffer, sample_count);
    default:
      return true;  // no analog output selected (USB only), nothing to write, not a failure
  }
}

// ---- Output buffer: deliberate slack between render/mix and the actual
// hardware write, see setOutBufferBlocks's own comment in audio_io.h. A
// ring of up to kMaxQueueBlocks+1 slots (+1 so the depth-0 case, today's
// exact direct-write behavior, still flows through one uniform push then
// immediately pop path rather than a special case). ----
constexpr int kMaxQueueBlocks = 4;
int16_t *queueBuf[kMaxQueueBlocks + 1] = {};
size_t queueBufLen = 0;
int queueHead = 0, queueCount = 0;
volatile int queueDepth = 0;
}  // namespace

bool begin() {
  const int pref = readPref();
  activeUsb = (pref == 0 || pref == 2 || pref == 4);
  activeAnalog = (pref == 1 || pref == 2)   ? ANALOG_MODULE
                 : (pref == 3 || pref == 4) ? ANALOG_INTERNAL
                                            : ANALOG_NONE;

  if (activeAnalog == ANALOG_NONE) return true;
  if (activeAnalog == ANALOG_INTERNAL) return beginInternalSpeaker();

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
  // codec's line input, speaker/headphone output live.
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
  if (activeAnalog != ANALOG_MODULE) return false;  // no aux input without ModuleAudio, see audio_io.h
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
  const size_t need = sample_count * 2;
  if (queueBufLen != need) {
    for (auto &b : queueBuf) {
      delete[] b;
      b = new int16_t[need];
    }
    queueBufLen = need;
    queueHead = queueCount = 0;
  }
  const int depth = queueDepth < 0 ? 0 : (queueDepth > kMaxQueueBlocks ? kMaxQueueBlocks : queueDepth);
  const int tail = (queueHead + queueCount) % (kMaxQueueBlocks + 1);
  memcpy(queueBuf[tail], buffer, need * sizeof(int16_t));
  queueCount++;
  // Still banking reserve, nothing to write to hardware yet. A depth
  // change (or first boot) drains or fills toward the new target a block
  // at a time rather than all at once, see setOutBufferBlocks's comment,
  // that's deliberate, not a bug.
  if (queueCount <= depth) return true;
  const int16_t *out = queueBuf[queueHead];
  queueHead = (queueHead + 1) % (kMaxQueueBlocks + 1);
  queueCount--;
  return writeToActiveBackend(out, sample_count);
}

int outputPref() { return readPref(); }

void setOutputPref(int v) {
  if (v < 0 || v > 4) return;
  Preferences p;
  p.begin(kPrefNamespace, /*readOnly=*/false);
  p.putInt(kPrefKey, v);
  p.end();
}

bool usbAudioEnabled() { return activeUsb; }

void setOutBufferBlocks(int n) { queueDepth = n < 0 ? 0 : (n > kMaxQueueBlocks ? kMaxQueueBlocks : n); }

// A software reset (esp_restart()) does not gracefully wind down
// peripherals first, whatever the codec or internal speaker's DAC/amp was
// last outputting is abruptly cut off mid stream, which is what a loud
// pop on reboot usually is, common enough on audio hardware generally
// that muting before a deliberate reset is the standard fix, not specific
// to this board. Call right before esp_restart() (the Audio Out picker's
// Apply button is the only place that calls it today). The delay is the
// codec/amp's analog output stage settling, milliseconds, not a
// meaningful addition to how long Apply already takes.
void muteBeforeReboot() {
  if (activeAnalog == ANALOG_MODULE)
    device.setMute(true);
  else if (activeAnalog == ANALOG_INTERNAL)
    M5.Speaker.stop();
  // Widened from 50ms: still popped on Ext specifically, right before the
  // new session's own boot sequence, reported on real hardware. Longer
  // settle time is the only additional lever this specific call has, a
  // codec's DAC/analog output stage settling is not something register
  // level muting can force to finish faster. If this still doesn't fully
  // clear it, this codec most likely just does not have a dedicated mute
  // relay/soft-start circuit and some residual pop on a hard reset may be
  // an inherent hardware limitation, not a software one.
  delay(150);
}

}  // namespace audio_io
