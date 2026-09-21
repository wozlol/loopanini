#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// The CoreS3 LCD and the Module USB (MAX3421E) share the SPI data and clock
// pads (GPIO35/36/37), only their chip selects differ. Anything that talks to
// either must hold this lock so two tasks never drive the shared pads at once.
namespace spi_lock {

inline SemaphoreHandle_t mutex = nullptr;

inline void init() {
  if (mutex == nullptr) mutex = xSemaphoreCreateRecursiveMutex();
}

struct Guard {
  Guard() {
    if (mutex) xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
  }
  ~Guard() {
    if (mutex) xSemaphoreGiveRecursive(mutex);
  }
};

}  // namespace spi_lock
