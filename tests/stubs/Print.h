#pragma once
// Host-side stand-in for Arduino's Print: just captures everything printed so
// the test can assert on it.
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <string>

class Print {
 public:
  std::string captured;
  size_t printf(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n > 0) captured.append(buf, static_cast<size_t>(n));
    return n > 0 ? static_cast<size_t>(n) : 0;
  }
  size_t println(const char *s) {
    captured += s;
    captured += "\n";
    return std::string(s).size() + 1;
  }
};
