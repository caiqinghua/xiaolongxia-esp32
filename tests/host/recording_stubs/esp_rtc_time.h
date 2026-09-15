#pragma once
#include "esp_timer.h"
inline uint64_t esp_rtc_get_time_us() { return static_cast<uint64_t>(test_time_us); }
