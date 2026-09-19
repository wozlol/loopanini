#pragma once

#include <cstdint>

#include <Print.h>

// Our own USB CDC serial interface, registered and started before the one
// shared USB.begin() call, alongside midi_io and usb_audio_out.
//
// Deliberately not the Serial/USBSerial macro pair, and requires "USB CDC
// On Boot" set to Disabled. With that board option Enabled, the core
// auto-starts a CDC-only USB connection before setup() even runs (see
// main.cpp's ARDUINO_USB_CDC_ON_BOOT block), and our own code adding MIDI
// and Audio interfaces afterward then forces a disruptive re-enumeration
// mid-boot, which is what was killing the Serial Monitor connection right
// as USB MIDI came up. Registering all three interfaces ourselves, together,
// before the single USB.begin() call gives one clean enumeration instead,
// with no mid-boot reconnect.
//
// Writes never block: with nobody listening, or the host not draining fast
// enough, output is dropped rather than stalling the caller. That is what
// makes it safe to print from the real-time audio task.
namespace debug_io {

// Registers the CDC interface. Call before the shared USB.begin() in
// loopanini.ino, alongside midi_io::begin() and usb_audio_out::begin().
// Also routes ESP-IDF's own error logs (driver failures) to this port.
void begin();

// The stream to print debug output to, in place of Serial.
Print &out();

// True while a terminal has the port open.
bool connected();

// Waits up to timeout_ms for a terminal to open the port, so boot messages
// aren't printed into the void. Returns whether one did. Never waits when a
// terminal is already attached.
bool waitForHost(uint32_t timeout_ms);

// Bytes typed into the terminal, for the tiny command interface in
// loopanini.ino. available() is a count, read() returns -1 if empty.
int available();
int read();

}  // namespace debug_io
