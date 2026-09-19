#pragma once

#include <cstddef>

// Boot-time GPIO ownership check.
//
// Every subsystem that is about to touch GPIOs claims all of them here
// first. If any pin already belongs to something else, the whole claim is
// refused, a loud message names every conflict, and the caller must skip
// whatever it was about to do. This exists because the CoreS3 stack shares
// nets between modules (see the pin budget in config.h): during phase 2 the
// USB host chip select landed on ModuleAudio's master clock pin and silently
// turned the codec's output into noise. With this guard that becomes a
// printed refusal, not a mystery, and the conflicting subsystem simply
// doesn't start.
//
// Claim order decides who wins, so claim in priority order: audio first.
namespace pin_guard {

struct PinClaim {
  int gpio;
  const char *owner;  // must be a string literal, only the pointer is stored
};

// All or nothing. Returns true and records every pin if none is taken. If any
// pin is already owned by a different owner, or isn't a real GPIO number,
// records NOTHING (so a refused subsystem never blocks pins for others),
// prints every conflict on the debug CDC, and returns false. The same owner
// asking again for the same pin is not a conflict. Safe from any task.
bool claimAll(const PinClaim *pins, size_t count);

// Convenience for a single pin.
bool claim(int gpio, const char *owner);

// Prints every recorded claim, for the boot log.
void printTable();

}  // namespace pin_guard
