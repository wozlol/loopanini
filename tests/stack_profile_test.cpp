// Host-side test of the intended CoreS3 stack profile, using the real pin
// numbers: ModuleAudio B, Module USB v1.2 on SS Select CH2 + INT Select CH1,
// and the Unit MIDI on Port B (the default) or Port C. All three must coexist. Plugging the
// Unit MIDI into Port A (red) must be refused, because GPIO1 is the chip
// select. Build and run with tests/run_tests.sh.
#include <cstdio>
#include <string>

#include "Print.h"
#include "pin_guard.h"

namespace debug_io {
Print &out() {
  static Print p;
  return p;
}
}  // namespace debug_io

static int failures = 0;
#define CHECK(cond)                                           \
  do {                                                        \
    if (!(cond)) {                                            \
      std::printf("FAIL line %d: %s\n", __LINE__, #cond);     \
      ++failures;                                             \
    }                                                         \
  } while (0)

static std::string take_output() {
  std::string s = debug_io::out().captured;
  debug_io::out().captured.clear();
  return s;
}

int main() {
  using pin_guard::PinClaim;

  // Claimed in the same order as the firmware: audio, then DIN, then host.
  const PinClaim audio[] = {{6, "ModuleAudio I2S LRCK"}, {7, "ModuleAudio I2S MCLK"},
                            {13, "ModuleAudio I2S DOUT"}, {0, "ModuleAudio I2S BCK"},
                            {14, "ModuleAudio I2S DIN"}};
  const PinClaim din_port_c[] = {{18, "DIN MIDI in (UART RX)"}, {17, "DIN MIDI out (UART TX)"}};
  const PinClaim host_ch2_int_ch1[] = {{1, "USB host chip select (Module USB SS)"},
                                       {10, "USB host INT (Module USB INT)"}};

  CHECK(pin_guard::claimAll(audio, 5));
  CHECK(pin_guard::claimAll(din_port_c, 2));
  CHECK(pin_guard::claimAll(host_ch2_int_ch1, 2));
  CHECK(take_output().empty());  // nothing complained

  // The Unit MIDI in Port A (red) would drive the chip select. With host
  // already running it must be refused, and the message must name GPIO1.
  const PinClaim din_port_a[] = {{1, "DIN MIDI in (UART RX)"}, {2, "DIN MIDI out (UART TX)"}};
  CHECK(!pin_guard::claimAll(din_port_a, 2));
  std::string out = take_output();
  CHECK(out.find("GPIO1 ") != std::string::npos);
  CHECK(out.find("USB host chip select") != std::string::npos);
  // ...and refusing it must not have kept GPIO2 for a subsystem that didn't start.
  CHECK(pin_guard::claim(2, "probe on GPIO2"));

  // Port B (black) is also fine for the Unit MIDI, as an alternative to Port C.
  const PinClaim din_port_b[] = {{8, "DIN MIDI in (UART RX)"}, {9, "DIN MIDI out (UART TX)"}};
  CHECK(pin_guard::claimAll(din_port_b, 2));

  if (failures == 0) std::printf("stack_profile_test: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
