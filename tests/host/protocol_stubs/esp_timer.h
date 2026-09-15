#pragma once
#include <cstdint>
inline int64_t host_monotonic_ms = 1000;
inline int64_t esp_timer_get_time() { return host_monotonic_ms * 1000; }
