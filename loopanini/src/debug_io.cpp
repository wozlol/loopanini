#include "debug_io.h"

#include <cstdarg>
#include <cstdio>

#include <Arduino.h>
#include <esp_log.h>

#include "USBCDC.h"

namespace debug_io {

namespace {

USBCDC cdc(0);

// ESP-IDF's own driver errors (I2S, GPIO, SPI...) normally go to UART0, which
// isn't wired to anything useful on this stack, so without this hook a failing
// driver call is invisible. Log level is compiled at ERROR only for this
// board (CONFIG_LOG_MAXIMUM_LEVEL 1), so what arrives here is just the errors,
// which is exactly what's wanted.
//
// Rate limited: a failure that repeats once per audio block must not flood the
// port or spend the audio task's time formatting text. Never blocks either,
// see setTxTimeoutMs(0) in begin().
int logToCdc(const char *fmt, va_list args) {
  constexpr uint32_t kMaxLinesPerSecond = 20;
  static uint32_t window_start_ms = 0;
  static uint32_t lines_in_window = 0;

  const uint32_t now = millis();
  if (now - window_start_ms >= 1000) {
    window_start_ms = now;
    lines_in_window = 0;
  }
  if (lines_in_window >= kMaxLinesPerSecond) return 0;
  ++lines_in_window;

  char buf[192];
  int n = vsnprintf(buf, sizeof(buf), fmt, args);
  if (n > 0) {
    size_t len = static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n) : sizeof(buf) - 1;
    cdc.write(reinterpret_cast<const uint8_t *>(buf), len);
  }
  return n;
}

}  // namespace

void begin() {
  // Never block or spin the caller. The default 250 ms TX timeout makes
  // USBCDC::write() busy-wait, without yielding, whenever the host has the
  // port open but isn't draining it fast enough, and a debug print from the
  // real-time audio task must never be able to do that. With 0, a write
  // that can't proceed immediately is simply dropped.
  cdc.setTxTimeoutMs(0);
  cdc.begin(115200);
  esp_log_set_vprintf(logToCdc);
}

Print &out() { return cdc; }

bool connected() { return static_cast<bool>(cdc); }

bool waitForHost(uint32_t timeout_ms) {
  const uint32_t start = millis();
  while (!connected() && (millis() - start) < timeout_ms) {
    delay(10);
  }
  return connected();
}

int available() { return cdc.available(); }

int read() { return cdc.read(); }

}  // namespace debug_io
