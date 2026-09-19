#pragma once
// Host-side stand-in for the bits of FreeRTOS that pin_guard.cpp uses. On the
// bench there is one thread, so the spinlock is a no-op.
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
