#include "midi_io.h"

#include <cstddef>
#include <cstdint>

#include "USB.h"
#include "USBMIDI.h"

#include "debug_io.h"

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
    // upstream. Fine to remove once phase 1 is confirmed working.
    debug_io::out().printf("USB MIDI in: %02X %02X %02X\n", bytes[0], bytes[1], bytes[2]);
    convert_midi_bytes_to_messages(bytes, 3, /*usb=*/1);
  }
}

}  // namespace midi_io
