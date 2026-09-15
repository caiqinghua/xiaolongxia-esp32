#pragma once
inline void TestLinkLog(const char*, const char*, ...) {}
#define ESP_LOGI(...) TestLinkLog(__VA_ARGS__)
