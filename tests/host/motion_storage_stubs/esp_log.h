#pragma once

#include <cstdarg>
#include <cstdio>

inline void WatchMotionTestLog(const char* tag, const char* format, ...) {
    std::fprintf(stderr, "%s: ", tag);
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
    std::fputc('\n', stderr);
}

#define ESP_LOGE(...) WatchMotionTestLog(__VA_ARGS__)
