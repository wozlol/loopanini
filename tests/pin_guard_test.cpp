// Host-side test for src/pin_guard.cpp. Build and run with tests/run_tests.sh.
// Replays the real scenarios on the CoreS3 stack: ModuleAudio B claims its
// pins, then USB host and DIN MIDI try to start on top of it.
#include <cstdio>
#include <cstdlib>
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
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("FAIL line %d: %s\n", __LINE__, #cond);                    \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static std::string take_output() {
  std::string s = debug_io::out().captured;
  debug_io::out().captured.clear();
  return s;
}
static bool contains(const std::string &hay, const char *needle) {
  return hay.find(needle) != std::string::npos;
}

int main() {
  using pin_guard::PinClaim;

  // ModuleAudio position B on CoreS3: LRCK 6, MCLK 7, DOUT 13, BCK 0, DIN 14.
  const PinClaim audio[] = {{6, "ModuleAudio I2S LRCK"}, {7, "ModuleAudio I2S MCLK"},
                            {13, "ModuleAudio I2S DOUT"}, {0, "ModuleAudio I2S BCK"},
                            {14, "ModuleAudio I2S DIN"}};
  CHECK(pin_guard::claimAll(audio, 5));
  CHECK(take_output().empty());

  // The phase 2 bug: Module USB on SS Select CH1 (GPIO7) + INT CH1 (GPIO10).
  const PinClaim host_ch1[] = {{7, "USB host chip select (Module USB SS)"},
                               {10, "USB host INT (Module USB INT)"}};
  CHECK(!pin_guard::claimAll(host_ch1, 2));
  std::string out = take_output();
  CHECK(contains(out, "PIN CONFLICT"));
  CHECK(contains(out, "GPIO7"));
  CHECK(contains(out, "ModuleAudio I2S MCLK"));
  CHECK(contains(out, "USB host chip select"));
  // All or nothing: the refused host must not have kept GPIO10 for itself.
  CHECK(pin_guard::claim(10, "probe on GPIO10"));

  // DIN MIDI on Port A: RX GPIO1, TX GPIO2. Free, so it starts.
  const PinClaim din[] = {{1, "DIN MIDI in (UART RX, Port A)"}, {2, "DIN MIDI out (UART TX, Port A)"}};
  CHECK(pin_guard::claimAll(din, 2));

  // Host on the only audio safe position, SS Select CH2 (GPIO1), with DIN
  // already running: refused, because GPIO1 is DIN's RX.
  const PinClaim host_ch2[] = {{1, "USB host chip select (Module USB SS)"},
                               {21, "USB host INT (test pin)"}};
  CHECK(!pin_guard::claimAll(host_ch2, 2));
  out = take_output();
  CHECK(contains(out, "GPIO1 "));
  CHECK(contains(out, "DIN MIDI in"));
  CHECK(pin_guard::claim(21, "probe on GPIO21"));

  // Same owner asking again is not a conflict.
  CHECK(pin_guard::claim(6, "ModuleAudio I2S LRCK"));
  CHECK(take_output().empty());

  // Not real pins.
  CHECK(!pin_guard::claim(-1, "negative"));
  CHECK(!pin_guard::claim(49, "too high"));
  CHECK(!pin_guard::claim(255, "unavailable sentinel"));
  CHECK(contains(take_output(), "not a real pin"));

  // A request can't claim one pin for two owners, and refuses atomically.
  const PinClaim twice[] = {{30, "owner a"}, {30, "owner b"}};
  CHECK(!pin_guard::claimAll(twice, 2));
  take_output();
  CHECK(pin_guard::claim(30, "probe on GPIO30"));

  // The same owner listing a pin twice in one request is fine, recorded once.
  const PinClaim dup_same[] = {{31, "owner x"}, {31, "owner x"}};
  CHECK(pin_guard::claimAll(dup_same, 2));

  // Fill the table (32 claims). Used so far: 5 audio + 10 + 2 DIN + 21 + 30 + 31 = 11.
  int used = 11;
  for (int gpio = 0; gpio <= 48 && used < 32; ++gpio) {
    if (pin_guard::claim(gpio, "filler")) ++used;
  }
  CHECK(used == 32);
  take_output();
  // Table is full and 33rd distinct pin is refused. Find a GPIO not yet taken.
  bool refused_one = false;
  for (int gpio = 0; gpio <= 48; ++gpio) {
    if (!pin_guard::claim(gpio, "one too many")) {
      if (contains(take_output(), "table full")) { refused_one = true; break; }
    }
  }
  CHECK(refused_one);

  if (failures == 0) std::printf("pin_guard_test: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
