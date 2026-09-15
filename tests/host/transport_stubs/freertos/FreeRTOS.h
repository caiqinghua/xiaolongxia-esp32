#pragma once
#include <cstdint>
using EventBits_t = uint32_t;
using TickType_t = uint32_t;
using BaseType_t = int;
constexpr bool pdTRUE = true;
constexpr bool pdFALSE = false;
constexpr int pdPASS = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr EventBits_t BIT0 = 1;
constexpr EventBits_t BIT1 = 2;
#define pdMS_TO_TICKS(ms) (ms)
