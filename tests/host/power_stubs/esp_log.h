#pragma once
#include <cstdarg>
inline void PowerTestLog(const char*, const char*, ...) {}
#define ESP_LOGI(...) PowerTestLog(__VA_ARGS__)
#define ESP_LOGW(...) PowerTestLog(__VA_ARGS__)
#define ESP_LOGE(...) PowerTestLog(__VA_ARGS__)
#define ESP_LOGD(...) PowerTestLog(__VA_ARGS__)
