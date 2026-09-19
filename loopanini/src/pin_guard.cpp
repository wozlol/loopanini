#include "pin_guard.h"

#include <cstddef>
#include <cstring>

#include <freertos/FreeRTOS.h>

#include "debug_io.h"

namespace pin_guard {

namespace {

constexpr size_t kMaxClaims = 32;
constexpr size_t kMaxRequest = 16;
constexpr int kMaxGpio = 48;  // ESP32-S3 has GPIO0..GPIO48

PinClaim claims[kMaxClaims];
size_t claim_count = 0;
portMUX_TYPE claim_lock = portMUX_INITIALIZER_UNLOCKED;

enum class Verdict : unsigned char { kFree, kSameOwner, kTaken, kInvalid };

// Which claim, if any, blocks `request`. Looks at the recorded table and at
// the earlier entries of the same request, so a request can't quietly claim
// one pin for two owners. Call with claim_lock held.
Verdict check(const PinClaim &request, const PinClaim *earlier, size_t earlier_count,
              const char **blocked_by) {
  if (request.gpio < 0 || request.gpio > kMaxGpio) return Verdict::kInvalid;
  for (size_t i = 0; i < claim_count; ++i) {
    if (claims[i].gpio == request.gpio) {
      *blocked_by = claims[i].owner;
      return std::strcmp(claims[i].owner, request.owner) == 0 ? Verdict::kSameOwner : Verdict::kTaken;
    }
  }
  for (size_t i = 0; i < earlier_count; ++i) {
    if (earlier[i].gpio == request.gpio) {
      *blocked_by = earlier[i].owner;
      return std::strcmp(earlier[i].owner, request.owner) == 0 ? Verdict::kSameOwner : Verdict::kTaken;
    }
  }
  return Verdict::kFree;
}

}  // namespace

bool claimAll(const PinClaim *pins, size_t count) {
  if (count == 0) return true;
  if (count > kMaxRequest) {
    debug_io::out().printf("PIN GUARD: a claim of %u pins is larger than the guard supports.\n",
                            static_cast<unsigned>(count));
    return false;
  }

  Verdict verdicts[kMaxRequest];
  const char *blocked_by[kMaxRequest];
  bool ok = true;
  bool table_full = false;

  portENTER_CRITICAL(&claim_lock);
  size_t new_pins = 0;
  for (size_t i = 0; i < count; ++i) {
    blocked_by[i] = nullptr;
    verdicts[i] = check(pins[i], pins, i, &blocked_by[i]);
    if (verdicts[i] == Verdict::kTaken || verdicts[i] == Verdict::kInvalid) ok = false;
    if (verdicts[i] == Verdict::kFree) ++new_pins;
  }
  if (ok && claim_count + new_pins > kMaxClaims) {
    ok = false;
    table_full = true;
  }
  if (ok) {
    for (size_t i = 0; i < count; ++i) {
      if (verdicts[i] == Verdict::kFree) claims[claim_count++] = pins[i];
    }
  }
  portEXIT_CRITICAL(&claim_lock);

  // Print outside the lock: a USB write inside a spinlock would stall the
  // other core.
  if (!ok) {
    for (size_t i = 0; i < count; ++i) {
      if (verdicts[i] == Verdict::kInvalid) {
        debug_io::out().printf("PIN GUARD: \"%s\" asked for GPIO%d, which is not a real pin.\n",
                                pins[i].owner, pins[i].gpio);
      } else if (verdicts[i] == Verdict::kTaken) {
        debug_io::out().printf(
            "PIN CONFLICT: GPIO%d is already used by \"%s\", so \"%s\" was refused and will not "
            "start.\n",
            pins[i].gpio, blocked_by[i], pins[i].owner);
      }
    }
    if (table_full) debug_io::out().println("PIN GUARD: claim table full, request refused.");
  }
  return ok;
}

bool claim(int gpio, const char *owner) {
  const PinClaim one = {gpio, owner};
  return claimAll(&one, 1);
}

void printTable() {
  // Copy under the lock, print outside it, same reason as above.
  PinClaim snapshot[kMaxClaims];
  size_t n;
  portENTER_CRITICAL(&claim_lock);
  n = claim_count;
  for (size_t i = 0; i < n; ++i) snapshot[i] = claims[i];
  portEXIT_CRITICAL(&claim_lock);

  debug_io::out().println("pin guard: GPIOs in use by loopanini:");
  for (size_t i = 0; i < n; ++i) {
    debug_io::out().printf("  GPIO%-2d  %s\n", snapshot[i].gpio, snapshot[i].owner);
  }
}

}  // namespace pin_guard
