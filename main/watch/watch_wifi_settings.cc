#include "watch_wifi_settings.h"
#include "nvs.h"

int ReadWatchWifiEnabled() {
    nvs_handle_t handle = 0;
    auto result = nvs_open("audream_watch", NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return 1;
    if (result != ESP_OK) return -1;
    int32_t value = 1;
    result = nvs_get_i32(handle, "wifi_enabled", &value);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return 1;
    if (result != ESP_OK || (value != 0 && value != 1)) return -1;
    return value;
}
bool SaveWatchWifiEnabled(bool enabled) {
    nvs_handle_t handle = 0;
    auto result = nvs_open("audream_watch", NVS_READWRITE, &handle);
    if (result != ESP_OK) return false;
    result = nvs_set_i32(handle, "wifi_enabled", enabled ? 1 : 0);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}
