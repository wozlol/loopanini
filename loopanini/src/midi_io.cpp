#include "midi_io.h"

#include <cstddef>
#include <cstdint>

#include "USB.h"
#include "USBMIDI.h"

#include "debug_io.h"
#include "sample_bank.h"
#include "synth_engine.h"

// Declared the same way AMY's own AMY_USB_Host_MIDI example declares it:
// amy_midi.h isn't part of AMY-Arduino.h's public include chain, so this
// local extern "C" is the documented way to reach AMY's byte-level MIDI
// parser from outside the library.
extern "C" {
void convert_midi_bytes_to_messages(uint8_t *data, size_t len, uint8_t usb);
}

namespace midi_io {

namespace {
// Named explicitly so the host OS and DAWs show "Loopanini" for the MIDI
// port rather than a generic default. USB.productName() in loopanini.ino
// sets the equivalent name for the device as a whole.
USBMIDI usb_midi("Loopanini");

// Captures a patch sent as SYSEX (the AMYboard/Tulip web editor's own way
// to push a patch to a running board, see docs/midi.md's SYSEX section)
// and saves it as a new Custom file, byte accumulated across USB-MIDI's
// own multi packet SYSEX framing (Code Index Number 0x4 continues, 0x5/6/7
// end with 1/2/3 bytes). AMY's manufacturer ID is 00 03 45 (also
// docs/midi.md); only sysex carrying it is treated as a patch, anything
// else is silently ignored rather than misrouted, this project isn't a
// general sysex bridge.
uint8_t sysexBuf[512];
size_t sysexLen = 0;
bool sysexActive = false;

void sysexByte(uint8_t b) {
  if (b == 0xF0) {
    sysexActive = true;
    sysexLen = 0;
    return;
  }
  if (!sysexActive) return;
  if (b == 0xF7) {
    sysexActive = false;
    if (sysexLen > 3 && sysexBuf[0] == 0x00 && sysexBuf[1] == 0x03 && sysexBuf[2] == 0x45) {
      // Wire messages are lower ASCII (docs/midi.md), safe to treat the
      // rest of the buffer as a plain C string once null terminated.
      sysexBuf[sysexLen] = 0;
      const bool ok = sample_bank::saveCustomPatch((const char *)(sysexBuf + 3));
      debug_io::out().printf("midi_io: AMY sysex patch received, %s.\n",
                              ok ? "saved to Custom" : "save failed");
    }
    return;
  }
  if (sysexLen < sizeof(sysexBuf) - 1) sysexBuf[sysexLen++] = b;
}

}  // namespace

void begin() {
  // USB.begin() is called once, centrally, in loopanini.ino, after every
  // composite USB class (this one, debug_io.h, and usb_audio_out.h) has
  // registered itself. Calling it here too would double-start the stack.
  usb_midi.begin();
}

void poll() {
  midiEventPacket_t packet;
  while (usb_midi.readPacket(&packet)) {
    if (packet.header == 0) continue;  // empty packet
    // USB-MIDI packing puts status in byte1, data bytes in byte2/byte3.
    // AMY's parser reads the status byte to know how many data bytes a
    // short message (Program Change, Channel Pressure) actually needs and
    // ignores the rest, so always passing all 3 is correct, not just
    // convenient, see AMY_USB_Host_MIDI.ino.
    uint8_t bytes[3] = {packet.byte1, packet.byte2, packet.byte3};
    // Bring-up instrumentation: confirms bytes are actually arriving here
    // and what AMY is being handed, versus a silent USB-only problem
    // upstream. Fine to remove once phase 1 is confirmed working. Real
    // time messages (0xF8-0xFF: clock, active sensing, ...) are skipped,
    // same reasoning as midi_host.cpp, some hosts send clock continuously
    // and it floods this line into unreadable spam, still fully
    // processed below either way.
    if (bytes[0] < 0xF8) {
      debug_io::out().printf("USB MIDI in: %02X %02X %02X\n", bytes[0], bytes[1], bytes[2]);
    }
    // Code Index Number (packet.header's low nibble): 0x4 SysEx
    // starts/continues (3 bytes), 0x5 ends with 1, 0x6 ends with 2, 0x7
    // ends with 3. Anything in that group is SysEx framing, not a normal
    // channel/system message, routed to the byte accumulator above
    // instead of AMY's 3-byte message parser.
    const uint8_t cin = packet.header & 0x0F;
    if (cin >= 0x4 && cin <= 0x7) {
      sysexByte(bytes[0]);
      if (cin == 0x4 || cin == 0x6 || cin == 0x7) sysexByte(bytes[1]);
      if (cin == 0x4 || cin == 0x7) sysexByte(bytes[2]);
      continue;
    }
    // Channel 10 notes covered by an SD loaded kit play from our own PCM
    // sample layer instead of AMY's baked in drum synth, see synth_engine.h.
    if (!synth_engine::routeDrumNote(bytes)) convert_midi_bytes_to_messages(bytes, 3, /*usb=*/1);
  }
}

}  // namespace midi_io
