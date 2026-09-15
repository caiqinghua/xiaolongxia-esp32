#pragma once
#include <cstddef>
constexpr unsigned MALLOC_CAP_INTERNAL = 1;
void* heap_caps_malloc(size_t, unsigned);
void heap_caps_free(void*);
