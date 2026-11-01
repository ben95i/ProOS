// =====================================================================================
// test/mocks/pico/mutex.h
// Host stand-in for the pico-sdk recursive mutex. The simulator runs both cores' loops in turn
// on one thread, so the mutex only counts its depth; the checks require it to be released
// after every pass.
// =====================================================================================
#pragma once

#include <stdint.h>

struct recursive_mutex_t {
  bool initialised;
  int32_t depth;
};

void recursive_mutex_init(recursive_mutex_t *mutex);
void recursive_mutex_enter_blocking(recursive_mutex_t *mutex);
void recursive_mutex_exit(recursive_mutex_t *mutex);
