# patches/, library change record

The patched library is **vendored** into `loopanini/src/USB_Host_Shield_Library_2.0/`.
That vendored copy is the live, self-contained source of truth, `src/midi_host.cpp`
includes it with quote-includes, so the sketch compiles from its own bundled
library and is immune to the global Arduino Library Manager ever updating or
reinstalling "USB Host Shield Library 2.0" out from under it. The global copy is
stock and untouched.

The `.patch` files here are the **record** of what was changed vs. stock
USB Host Shield Library 2.0 (v1.7.0, vendored 2026-09-16, regenerated
2026-09-18) with `diff -u`, for reference, review, and re-deriving the edits if
the library is ever re-vendored from a newer upstream. Exactly four library
files differ from stock, plus one rename:

- `USB_Host_Shield_2.0_CoreS3_pins_avrpins.patch` and
  `USB_Host_Shield_2.0_CoreS3_pins_UsbCore.patch`, the chip select and INT pins
  of the MAX3421E are `P_LOOPANINI_CS` / `P_LOOPANINI_INT`, built with `MAKE_PIN`
  from `LOOPANINI_USB_HOST_CS_GPIO` / `LOOPANINI_USB_HOST_INT_GPIO`, and
  `UsbCore.h`'s ESP32 entry is `typedef MAX3421e<P_LOOPANINI_CS, P_LOOPANINI_INT>`.
  Stock is CS = GPIO5 / INT = GPIO17, neither of which is a CoreS3 switch option.
  The two macros come from `loopanini/src/config.h`, pulled in by
  `avrpins.h` with `#include "../config.h"`. That is deliberate: every
  translation unit of this library reaches `avrpins.h` through `Usb.h`, so they
  all see the same pins, whereas a `-D` or a macro defined only in
  `midi_host.cpp` would leave the library's other `.cpp` files with a different
  `MAX3421E` type, which is undefined behavior. **Read the pin budget at the top
  of `config.h` before changing them.** On CoreS3 with ModuleAudio, the only DIP
  setting that avoids a collision is SS Select CH2 (GPIO1) + INT Select CH1
  (GPIO10). SS Select CH1 (GPIO7) is ModuleAudio's MCLK.
- `USB_Host_Shield_2.0_bounded_Init_waits_usbhost.patch`, `Init()` and
  `Init(int)` each end with `while(!(regRd(rHCTL) & bmSAMPLEBUS));`, an unbounded
  spin on a register read. If the SPI link returns zeros (wrong or shared chip
  select, module missing) it never exits and hangs its core. Now bounded to 50 ms
  and `Init()` returns -1, which `midi_host.cpp` reports with the switch settings
  to check.
- `USB_Host_Shield_2.0_OutTransfer_timeout_Usb.patch`, the same two
  `while(!(regRd(rHIRQ) & bmHXFRDNIRQ))` completion spins in `USB::OutTransfer()`
  that arpnmidi already bounds (`arpnmidi/patches/USB_Host_Shield_2.0_OutTransfer_timeout.patch`,
  there for a hard freeze under bus stress), same 50 ms ceiling, same change.
- `BTD 2.h` renamed to `BTD2.h`. A stray duplicate shipped in the library folder
  with a space in the name, which Arduino IDE refuses. We don't use Bluetooth, no
  code includes it. Not a patch, just noted so the vendored folder doesn't look
  like it drifted.

Nothing else in the vendored copy was edited. `examples/` and `doc/` were left
out when vendoring.

## Why the SPI pins aren't patched

`MOSI`/`MISO`/`SCK` are not part of the template, the library calls `SPI.begin()`
with no arguments and the ESP32 Arduino core takes the board variant's defaults.
For `m5stack_cores3` those are `MOSI=37 MISO=35 SCK=36` (`pins_arduino.h`), which
match the Module USB v1.2's fixed SPI wiring on CoreS3 exactly, so nothing to do.
They are shared with the CoreS3's LCD and SD card by design (LCD on `SPI3_HOST`,
this library on `SPI2_HOST`, one shared set of pads).

## Re-vendoring from a fresh library (if ever needed)

1. Apply the four patches above to a clean USB Host Shield Library 2.0
   (`patch -p1` from inside the library folder, the headers use `a/` and `b/`).
2. Rename `BTD 2.h` to `BTD2.h` if that file is still there.
3. Copy it into `loopanini/src/USB_Host_Shield_Library_2.0/` (excluding
   `examples/`, `doc/`, `*.orig`).
4. Confirm `src/midi_host.cpp` still uses quote-includes to that vendored path.
5. Verify: temporarily rename the global
   `~/Documents/Arduino/libraries/USB_Host_Shield_Library_2.0` aside and
   `arduino-cli compile` the sketch, it must build with exit 0 from the
   vendored copy alone, then restore the global copy.
