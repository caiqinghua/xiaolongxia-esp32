#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
inline uint32_t esp_random() { return 1234; }
inline void esp_fill_random(void* output, size_t size) { std::memset(output, 0x34, size); }
