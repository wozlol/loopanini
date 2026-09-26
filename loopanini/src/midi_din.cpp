#include "midi_din.h"

#include <cstdint>

#include <HardwareSerial.h>
#include <driver/gpio.h>
#include <M5Unified.h>

#include "config.h"
#include "debug_io.h"
#include "pin_guard.h"

// Declared the same way midi_io.cpp does, see that file's comment. This is
// AMY's byte-at-a-time streaming parser rather than
// convert_midi_bytes_to_messages(), since DIN MIDI arrives as a continuous
// serial stream with running status, not discrete pre-framed packets like
// USB MIDI.
extern "C" {
void amy_process_single_midi_byte(uint8_t byte, uint8_t from_web_or_usb);
}

namespace midi_din {

namespace {
HardwareSerial din(LOOPANINI_DIN_MIDI_UART_NUM);
// Written once by begin() on the setup task, read by poll() on the audio
// task. volatile is enough for a one-way flag set before the audio task
// ever polls.
volatile bool active = false;
}  // namespace

bool begin() {
#if !LOOPANINI_ENABLE_DIN_MIDI
  debug_io::out().println("midi_din: disabled in config.h (LOOPANINI_ENABLE_DIN_MIDI 0).");
  return false;
#else
  // Every Grove port on the CoreS3 stack has the same pin order: pin 1 is the
  // UART RX and pin 2 is the TX from CoreS3's side, the Unit MIDI's TX wire
  // lands on pin 1 and its RX wire on pin 2. That order was confirmed on
  // real hardware on Port A, where DIN notes audibly changed AMY's output, and
  // it is also how M5Unified names Port C (pin 1 = RXD, pin 2 = TXD).
#if LOOPANINI_DIN_MIDI_PORT == LOOPANINI_PORT_A
  const char *port_name = "Port A (red)";
  const int core_rx = M5.getPin(m5::pin_name_t::port_a_pin1);
  const int core_tx = M5.getPin(m5::pin_name_t::port_a_pin2);
#elif LOOPANINI_DIN_MIDI_PORT == LOOPANINI_PORT_B
  const char *port_name = "Port B (black)";
  const int core_rx = M5.getPin(m5::pin_name_t::port_b_pin1);
  const int core_tx = M5.getPin(m5::pin_name_t::port_b_pin2);
#else
  const char *port_name = "Port C (blue)";
  const int core_rx = M5.getPin(m5::pin_name_t::port_c_pin1);
  const int core_tx = M5.getPin(m5::pin_name_t::port_c_pin2);
#endif

  const pin_guard::PinClaim pins[] = {
      {core_rx, "DIN MIDI in (UART RX)"},
      {core_tx, "DIN MIDI out (UART TX)"},
  };
  if (!pin_guard::claimAll(pins, sizeof(pins) / sizeof(pins[0]))) {
    debug_io::out().printf(
        "midi_din: NOT started, its %s pins conflict with something else, see the message above.\n",
        port_name);
    return false;
  }

  din.begin(LOOPANINI_DIN_MIDI_BAUD, SERIAL_8N1, core_rx, core_tx);
  // MIDI idles high. With nothing plugged in the RX pin floats and reads noise.
  gpio_pullup_en((gpio_num_t)core_rx);
  active = true;
  debug_io::out().printf("midi_din: running on %s, RX GPIO%d, TX GPIO%d.\n", port_name, core_rx, core_tx);
  return true;
#endif
}

void poll() {
  if (!active) return;
  while (din.available()) {
    uint8_t b = static_cast<uint8_t>(din.read());
    // Real time bytes (0xF8-0xFF) not logged, same reasoning as the other
    // two transports, still fully processed below either way.
    if (b < 0xF8) debug_io::out().printf("DIN MIDI in: %02X\n", b);
    amy_process_single_midi_byte(b, /*from_web_or_usb=*/0);
  }
}

}  // namespace midi_din
