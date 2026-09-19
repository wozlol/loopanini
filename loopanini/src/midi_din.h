#pragma once

// DIN MIDI in and out over CoreS3's Port A, the only Grove-style port
// CoreS3 exposes externally, via the M5Stack Unit MIDI (SAM2695) plugged
// into it. The Unit MIDI's own onboard synth chip is deliberately unused,
// this is just the plain DIN MIDI in/out UART bridge it also provides,
// same role as the USB MIDI paths in midi_io.h and midi_host.h. See
// FIRMWARE_PLAN.md's Hardware section.
//
// Uses GPIO1 (RX) and GPIO2 (TX). GPIO1 is also the Module USB v1.2's only
// audio-safe chip select position, so this and midi_host.h are mutually
// exclusive on CoreS3, see the pin budget in config.h. Disabled with
// LOOPANINI_ENABLE_DIN_MIDI, and pin_guard refuses it on any conflict.
namespace midi_din {

// Returns true if DIN MIDI is now running. False if it's disabled in
// config.h or its pins were already claimed, in which case a message says
// why and poll() does nothing.
bool begin();

// Drains whatever DIN MIDI bytes have arrived since the last call and
// feeds each one into AMY's parser. Call every audio block from the audio
// task, same as midi_io::poll(). A no-op unless begin() returned true.
void poll();

}  // namespace midi_din
