#pragma once
inline void HostTransportLog(const char*, const char*, ...) {}
#define ESP_LOGI(...) HostTransportLog(__VA_ARGS__)
#define ESP_LOGW(...) HostTransportLog(__VA_ARGS__)
#define ESP_LOGE(...) HostTransportLog(__VA_ARGS__)
#define ESP_LOGD(...) HostTransportLog(__VA_ARGS__)
