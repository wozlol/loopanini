#include "midi_host.h"

#include <cstddef>
#include <cstdint>
#include <new>

// Quote-includes into the vendored, patched copy, not the global Arduino
// Library Manager install, see patches/README.md. usbh_midi.h itself pulls
// in Usb.h the same way, so the whole tree resolves from here down.
#include "USB_Host_Shield_Library_2.0/usbh_midi.h"

#include "config.h"
#include "debug_io.h"
#include "pin_guard.h"
#include "spi_lock.h"

// Declared the same way midi_io.cpp does, see that file's comment.
extern "C" {
void convert_midi_bytes_to_messages(uint8_t *data, size_t len, uint8_t usb);
}

namespace midi_host {

namespace {
// Created by begin() only after the pins are cleared, never at static-init
// time. The library's constructors don't touch pins (its own comments say
// hardware init belongs in Init()), but keeping construction here means no
// code path exists that could reach a pin the guard hasn't approved.
USB *usb = nullptr;
USBH_MIDI *midi = nullptr;
volatile bool ready = false;
// The library never clears USB::devConfig[] and assumes a zero-filled global.
// Heap memory is not zeroed, so a plain `new USB()` crashed in USB::Task().
alignas(USB) uint8_t usbStorage[sizeof(USB)];
}  // namespace

bool begin() {
#if !LOOPANINI_ENABLE_USB_HOST
  debug_io::out().println("midi_host: disabled in config.h (LOOPANINI_ENABLE_USB_HOST 0).");
  return false;
#else
  const pin_guard::PinClaim pins[] = {
      {LOOPANINI_USB_HOST_CS_GPIO, "USB host chip select (Module USB SS)"},
      {LOOPANINI_USB_HOST_INT_GPIO, "USB host INT (Module USB INT)"},
  };
  if (!pin_guard::claimAll(pins, sizeof(pins) / sizeof(pins[0]))) {
    debug_io::out().println(
        "midi_host: NOT started, its chip select or INT pin conflicts with something else, see the "
        "message above. Module USB v1.2 needs SS Select CH2 + INT Select CH1 next to ModuleAudio, "
        "see the pin budget in config.h.");
    return false;
  }

  usb = new (usbStorage) USB();
  midi = new USBH_MIDI(usb);
  spi_lock::Guard lock;
  if (usb->Init() == -1) {
    debug_io::out().println(
        "midi_host: MAX3421E did not answer. Check Module USB v1.2 is seated, its SS Select and INT "
        "Select switches match LOOPANINI_USB_HOST_CS_GPIO / _INT_GPIO in config.h (CH2 = GPIO1, "
        "CH1 = GPIO10).");
    return false;
  }

  const uint8_t rev = usb->regRd(rREVISION);
  debug_io::out().printf("midi_host: MAX3421E revision register = 0x%02X (a real chip reads 0x01, 0x12 or 0x13, 0xFF or 0x00 means SPI is not reaching it)\n", rev);
  if (rev == 0x00 || rev == 0xFF) {
    debug_io::out().println(
        "midi_host: NOT started, no answer from the chip. Check Module USB SS Select = middle switch (GPIO1) "
        "and INT Select = first switch (GPIO10), and that the module is fully seated.");
    return false;
  }
  ready = true;
  debug_io::out().println("midi_host: running.");
  return true;
#endif
}

void poll() {
  // Guard on the pointers themselves, not just `ready`: with the feature
  // disabled they are never assigned, and the compiler can see that.
  if (!ready || usb == nullptr || midi == nullptr) return;
  spi_lock::Guard lock;
  usb->Task();
  static uint8_t lastState = 0xFF, lastVbus = 0xFF;
  static bool lastConn = false;
  const uint8_t st = usb->getUsbTaskState();
  const uint8_t vb = usb->getVbusState();
  const bool conn = (bool)*midi;
  if (st != lastState || vb != lastVbus || conn != lastConn) {
    debug_io::out().printf("midi_host: task state 0x%02X, bus %u (0=SE0 none,1=SE1,2=full-speed,3=low-speed), midi device %s\n",
                           st, vb, conn ? "READY" : "not ready");
    lastState = st;
    lastVbus = vb;
    lastConn = conn;
  }
  if (!conn) return;  // no class compliant MIDI device currently enumerated

  uint8_t msg[3];
  uint8_t len;
  while ((len = midi->RecvData(msg)) > 0) {
    debug_io::out().printf("USB host MIDI in: %02X %02X %02X\n", msg[0],
                            len > 1 ? msg[1] : 0, len > 2 ? msg[2] : 0);
    convert_midi_bytes_to_messages(msg, len, /*usb=*/1);
  }
}

}  // namespace midi_host
