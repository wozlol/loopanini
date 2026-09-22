#include "sample_bank.h"

#include <cstring>

#include <AMY-Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <M5Unified.h>

#include "config.h"
#include "debug_io.h"

// pcm_load() is a public AMY C function (declared in amy.h) that allocates a
// PCM preset's sample RAM (from amy_config.ram_caps_sample, MALLOC_CAP_SPIRAM
// here, see synth_engine::begin) and returns a pointer for the caller to fill
// directly. It is the same function AMY's own wire protocol ('z' messages,
// used by amy.load_sample()) calls after it has already read a WAV off disk;
// we read the WAV ourselves with the SD library and fill the buffer the same
// way, which is the intended direct-C-API path, not a workaround.
extern "C" int16_t *pcm_load(uint16_t preset_number, uint32_t length, uint32_t samplerate,
                              uint8_t channels, float midinote, uint32_t loopstart, uint32_t loopend);
extern "C" void pcm_unload_preset(uint16_t preset_number);

namespace sample_bank {
namespace {
bool sdReady = false;
}

// Same pin lookup and fallback as M5Unified's own Speaker_SD_wav_file
// example: sd_spi_cs from the board's pin table, GPIO4 if the table doesn't
// have one, SD's own SPI bus started explicitly first (shared with the LCD
// on SPI3_HOST, see FIRMWARE_PLAN.md's Pin budget).
bool begin() {
  int cs = M5.getPin(m5::pin_name_t::sd_spi_cs);
  if (cs < 0) cs = GPIO_NUM_4;
  SPI.begin(M5.getPin(m5::pin_name_t::sd_spi_sclk), M5.getPin(m5::pin_name_t::sd_spi_miso),
            M5.getPin(m5::pin_name_t::sd_spi_mosi), cs);
  sdReady = SD.begin(cs, SPI, 25000000);
  if (!sdReady) debug_io::out().println("sample_bank: no SD card found.");
  return sdReady;
}

namespace {

// Minimal RIFF/WAVE reader: PCM (format 1) or WAVE_FORMAT_EXTENSIBLE with a
// PCM subformat, 16 bit only, mono or stereo. That covers ordinary sample
// pack exports; anything else (float, 24 bit, ADPCM, compressed) is refused
// with a message rather than played back wrong.
struct WavInfo {
  uint16_t channels = 0;
  uint32_t sampleRate = 0;
  uint16_t bits = 0;
  uint32_t dataBytes = 0;
  uint32_t dataOffset = 0;
};

bool readWavHeader(File &f, WavInfo *out) {
  uint8_t riff[12];
  if (f.read(riff, 12) != 12) return false;
  if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) return false;

  bool haveFmt = false;
  while (f.available() >= 8) {
    uint8_t hdr[8];
    if (f.read(hdr, 8) != 8) return false;
    const uint32_t chunkSize = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
    if (memcmp(hdr, "fmt ", 4) == 0) {
      uint8_t fmt[16];
      if (chunkSize < 16 || f.read(fmt, 16) != 16) return false;
      const uint16_t formatTag = fmt[0] | (fmt[1] << 8);
      out->channels = fmt[2] | (fmt[3] << 8);
      out->sampleRate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
      out->bits = fmt[14] | (fmt[15] << 8);
      // formatTag 1 = PCM, 0xFFFE = EXTENSIBLE (subformat not checked, we
      // just trust bits == 16 and refuse anything else below).
      if (formatTag != 1 && formatTag != 0xFFFE) return false;
      if (chunkSize > 16 && !f.seek(f.position() + (chunkSize - 16))) return false;
      haveFmt = true;
    } else if (memcmp(hdr, "data", 4) == 0) {
      out->dataBytes = chunkSize;
      out->dataOffset = f.position();
      if (haveFmt) return true;  // fmt always precedes data in a valid file
      if (!f.seek(f.position() + chunkSize)) return false;
    } else {
      if (!f.seek(f.position() + chunkSize + (chunkSize & 1))) return false;
    }
  }
  return false;
}

// Reads the whole data chunk into the PSRAM buffer pcm_load() handed back.
// AMY's sample_ram is interleaved int16 per frame, same as a PCM16 WAV's
// data chunk, so this is a straight copy once we know the frame count.
bool fillFromFile(File &f, const WavInfo &info, int16_t *dst, uint32_t frames) {
  f.seek(info.dataOffset);
  const uint32_t bytes = frames * info.channels * 2u;
  uint32_t got = 0;
  while (got < bytes) {
    const int n = f.read((uint8_t *)dst + got, bytes - got);
    if (n <= 0) return false;
    got += (uint32_t)n;
  }
  return true;
}

// Parses a leading MIDI note number off a filename: "36 Kick.wav",
// "36_Kick.wav", "36.wav" all give 36. No leading digits means -1, the file
// is skipped. Matches the convention SamplerBox and most drum sample packs
// use (folder = kit, filename = note number + name), see FIRMWARE_PLAN.md.
int noteFromFilename(const char *name) {
  int n = -1;
  int i = 0;
  while (name[i] >= '0' && name[i] <= '9') {
    if (n < 0) n = 0;
    n = n * 10 + (name[i] - '0');
    i++;
  }
  return (n >= 0 && n <= 127) ? n : -1;
}

int8_t drumLoaded[128];  // -1 = not loaded, else 1
constexpr int kMaxPitched = 3;
bool pitchedLoaded[kMaxPitched] = {false, false, false};

bool loadOneFile(File &f, uint16_t preset, float midinote) {
  WavInfo info;
  if (!readWavHeader(f, &info)) {
    debug_io::out().printf("sample_bank: %s is not a 16 bit PCM WAV, skipped.\n", f.name());
    return false;
  }
  if (info.bits != 16 || info.channels < 1 || info.channels > 2) {
    debug_io::out().printf("sample_bank: %s is %u bit / %u ch, only 16 bit mono or stereo is supported, skipped.\n",
                            f.name(), info.bits, info.channels);
    return false;
  }
  const uint32_t frames = info.dataBytes / (info.channels * 2u);
  if (frames == 0) return false;
  int16_t *ram = pcm_load(preset, frames, info.sampleRate, (uint8_t)info.channels, midinote, 0, 0);
  if (!ram) {
    debug_io::out().printf("sample_bank: no PSRAM left to load %s.\n", f.name());
    return false;
  }
  if (!fillFromFile(f, info, ram, frames)) {
    debug_io::out().printf("sample_bank: read error loading %s.\n", f.name());
    pcm_unload_preset(preset);
    return false;
  }
  return true;
}

}  // namespace

void unloadAll() {
  for (int n = 0; n < 128; n++)
    if (drumLoaded[n] > 0) pcm_unload_preset((uint16_t)(LOOPANINI_DRUM_PRESET_BASE + n));
  memset(drumLoaded, -1, sizeof(drumLoaded));
  for (int i = 0; i < kMaxPitched; i++)
    if (pitchedLoaded[i]) pcm_unload_preset((uint16_t)(LOOPANINI_PITCHED_PRESET_BASE + i));
  memset(pitchedLoaded, 0, sizeof(pitchedLoaded));
}

int loadDrumKit(const char *dir) {
  unloadAll();
  if (!sdReady) return 0;
  File d = SD.open(dir);
  if (!d || !d.isDirectory()) {
    debug_io::out().printf("sample_bank: %s not found on SD.\n", dir);
    return 0;
  }
  int count = 0;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (f.isDirectory()) {
      f.close();
      continue;
    }
    const char *name = f.name();
    const size_t len = strlen(name);
    const bool isWav = len > 4 && (strcasecmp(name + len - 4, ".wav") == 0);
    const int note = isWav ? noteFromFilename(name) : -1;
    if (note >= 0) {
      if (loadOneFile(f, (uint16_t)(LOOPANINI_DRUM_PRESET_BASE + note), (float)note)) {
        drumLoaded[note] = 1;
        count++;
      }
    }
    f.close();
  }
  d.close();
  debug_io::out().printf("sample_bank: loaded %d drum sample(s) from %s.\n", count, dir);
  return count;
}

bool hasDrum(int note) { return note >= 0 && note < 128 && drumLoaded[note] > 0; }
int drumPreset(int note) { return LOOPANINI_DRUM_PRESET_BASE + note; }

bool loadPitchedSample(int chIndex, const char *path) {
  if (chIndex < 0 || chIndex >= kMaxPitched) return false;
  if (!sdReady) return false;
  const uint16_t preset = (uint16_t)(LOOPANINI_PITCHED_PRESET_BASE + chIndex);
  if (pitchedLoaded[chIndex]) {
    pcm_unload_preset(preset);
    pitchedLoaded[chIndex] = false;
  }
  File f = SD.open(path);
  if (!f) {
    debug_io::out().printf("sample_bank: %s not found on SD.\n", path);
    return false;
  }
  const bool ok = loadOneFile(f, preset, 60.0f);  // native pitch defaults to C4
  f.close();
  pitchedLoaded[chIndex] = ok;
  return ok;
}

bool hasPitched(int chIndex) { return chIndex >= 0 && chIndex < kMaxPitched && pitchedLoaded[chIndex]; }
int pitchedPreset(int chIndex) { return LOOPANINI_PITCHED_PRESET_BASE + chIndex; }

}  // namespace sample_bank
