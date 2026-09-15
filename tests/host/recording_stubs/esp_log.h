#pragma once
template <typename... Args>
inline void IgnoreTestLog(const char*, const char*, Args...) {}
#define ESP_LOGE(...) IgnoreTestLog(__VA_ARGS__)
#define ESP_LOGW(...) IgnoreTestLog(__VA_ARGS__)
#define ESP_LOGI(...) IgnoreTestLog(__VA_ARGS__)
