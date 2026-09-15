#pragma once
#include <cstdint>
#include <cstring>
constexpr int ESP_OK = 0;
inline int esp_efuse_mac_get_default(uint8_t* output) {
    std::memset(output, 0x12, 6);
    return ESP_OK;
}
