#pragma once

// USB MIDI device input for phase 1. USB MIDI host (the stacked Module USB
// v1.2) and DIN MIDI are phase 2 and later, see FIRMWARE_PLAN.md's build
// phases.
namespace midi_io {

// Starts CoreS3's native USB port in MIDI device mode. Requires the board's
// USB Mode set to USB-OTG (TinyUSB) rather than the default Hardware CDC
// and JTAG, see FIRMWARE_PLAN.md's Toolchain section.
void begin();

// Drains whatever USB MIDI packets have arrived since the last call and
// feeds each one into AMY's parser. Call this every audio block from the
// audio task, see loopanini.ino.
void poll();

}  // namespace midi_io
