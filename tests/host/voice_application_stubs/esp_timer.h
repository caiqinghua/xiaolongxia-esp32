#pragma once
#include <cstdint>
inline int64_t test_voice_now_us = 10000000;
inline int64_t esp_timer_get_time() { return test_voice_now_us; }
