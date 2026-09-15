#pragma once

#include <cstdint>
#include <cstring>
#include <string>

using nvs_handle_t = unsigned;
using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_ERR_NVS_NOT_FOUND = 1;
constexpr int ESP_ERR_NVS_INVALID_LENGTH = 2;
constexpr int ESP_FAIL = -1;
constexpr int NVS_READONLY = 0;
constexpr int NVS_READWRITE = 1;

namespace fake_nvs {
inline std::string committed;
inline std::string pending;
inline bool present = false;
inline bool fail_open = false;
inline bool fail_read = false;
inline bool fail_set = false;
inline bool fail_commit = false;
inline unsigned handles = 0;
inline unsigned commits = 0;
inline std::string last_namespace;
inline std::string last_key;
}

inline esp_err_t nvs_open(const char* name, int mode, nvs_handle_t* handle) {
    fake_nvs::last_namespace = name;
    if (fake_nvs::fail_open) { return ESP_FAIL; }
    if (mode == NVS_READONLY && !fake_nvs::present) { return ESP_ERR_NVS_NOT_FOUND; }
    *handle = ++fake_nvs::handles;
    return ESP_OK;
}
inline void nvs_close(nvs_handle_t) { --fake_nvs::handles; fake_nvs::pending.clear(); }
inline esp_err_t nvs_get_str(nvs_handle_t, const char* key, char* buffer, size_t* size) {
    fake_nvs::last_key = key;
    if (fake_nvs::fail_read) { return ESP_FAIL; }
    if (!fake_nvs::present) { return ESP_ERR_NVS_NOT_FOUND; }
    const size_t needed = fake_nvs::committed.size() + 1;
    if (!buffer) { *size = needed; return ESP_OK; }
    if (*size < needed) { return ESP_ERR_NVS_INVALID_LENGTH; }
    std::memcpy(buffer, fake_nvs::committed.c_str(), needed);
    *size = needed;
    return ESP_OK;
}
inline esp_err_t nvs_set_str(nvs_handle_t, const char* key, const char* value) {
    fake_nvs::last_key = key;
    if (fake_nvs::fail_set) { return ESP_FAIL; }
    fake_nvs::pending = value;
    return ESP_OK;
}
inline esp_err_t nvs_commit(nvs_handle_t) {
    ++fake_nvs::commits;
    if (fake_nvs::fail_commit) { return ESP_FAIL; }
    fake_nvs::committed = fake_nvs::pending;
    fake_nvs::present = true;
    return ESP_OK;
}
