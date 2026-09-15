#pragma once
#include <cstdint>
using nvs_handle_t = unsigned;
constexpr int ESP_OK = 0;
constexpr int ESP_ERR_NVS_NOT_FOUND = 1;
constexpr int NVS_READONLY = 0;
constexpr int NVS_READWRITE = 1;
int nvs_open(const char* name, int mode, nvs_handle_t* handle);
int nvs_get_i32(nvs_handle_t handle, const char* key, int32_t* value);
int nvs_set_i32(nvs_handle_t handle, const char* key, int32_t value);
int nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);
