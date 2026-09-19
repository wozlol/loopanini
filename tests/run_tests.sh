#!/bin/sh
# Host-side tests, no hardware or Arduino toolchain needed.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
out="${TMPDIR:-/tmp}/loopanini_tests"
mkdir -p "$out"
for t in pin_guard_test stack_profile_test; do
  c++ -std=c++17 -Wall -Wextra -Werror \
    -I "$here/stubs" -I "$here/../loopanini/src" \
    "$here/../loopanini/src/pin_guard.cpp" "$here/$t.cpp" \
    -o "$out/$t"
  "$out/$t"
done
