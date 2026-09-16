#pragma once

template <typename... Args>
inline void EspLogErrorStub(const char* tag, const char* format, Args&&...) {
    (void)tag;
    (void)format;
}

#define ESP_LOGE(...) EspLogErrorStub(__VA_ARGS__)
#define esp_err_to_name(error) "error"
