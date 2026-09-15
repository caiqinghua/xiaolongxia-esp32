#pragma once

#include <assert.h>
#include <pthread.h>

typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(mux) do { int result = pthread_mutex_lock(mux); assert(result == 0); } while (0)
#define portEXIT_CRITICAL(mux) do { int result = pthread_mutex_unlock(mux); assert(result == 0); } while (0)
