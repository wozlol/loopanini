#pragma once

// USB MIDI host: a class compliant USB MIDI keyboard or controller plugged
// into the stacked M5Stack Module USB v1.2 (MAX3421E, SPI), separate from
// CoreS3's own native USB port, which stays in device mode (see midi_io.h).
// See FIRMWARE_PLAN.md's Hardware and Toolchain sections and patches/README.md
// for the vendored library patch.
//
// OFF by default (LOOPANINI_ENABLE_USB_HOST in config.h). On CoreS3 the
// module's SS Select CH1 sits on ModuleAudio's MCLK, and the only position
// that avoids ModuleAudio (CH2, GPIO1) shares GPIO1 with the Unit MIDI on
// Port A, so this and midi_din.h are mutually exclusive, see the pin budget
// in config.h. pin_guard refuses it on any conflict.
namespace midi_host {

// Claims the chip select and INT pins and initializes the MAX3421E. Returns
// true only if the host is up and poll() should run. False if it's disabled
// in config.h, its pins conflict, or the module didn't answer, in which case
// a message says which and nothing has touched a pin it wasn't cleared for.
// Call from the host task, not from the audio task, Init() is not bounded
// in time even with the vendored patch (50 ms waits, and it is SPI).
bool begin();

// Services the host stack and drains whatever MIDI bytes a connected
// device has sent since the last call, feeding each complete message into
// AMY's parser. A no-op unless begin() returned true.
void poll();

}  // namespace midi_host
