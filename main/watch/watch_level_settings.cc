#include "watch_level_settings.h"
#include "nvs.h"

namespace {
bool Known(WatchLevel kind) {
    return kind == WatchLevel::kBrightness || kind == WatchLevel::kVolume;
}
const char* Namespace(WatchLevel kind) { return kind == WatchLevel::kBrightness ? "display" : "audio"; }
const char* Key(WatchLevel kind) { return kind == WatchLevel::kBrightness ? "brightness" : "output_volume"; }
int Default(WatchLevel kind) { return kind == WatchLevel::kBrightness ? 75 : 70; }
}

bool IsValidWatchLevel(WatchLevel kind, int value) {
    return Known(kind) && value >= (kind == WatchLevel::kBrightness ? 10 : 0) && value <= 100;
}

int ReadWatchLevel(WatchLevel kind) {
    if (!Known(kind)) return -1;
    nvs_handle_t handle = 0;
    auto result = nvs_open(Namespace(kind), NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return Default(kind);
    if (result != ESP_OK) return -1;
    int32_t value = 0;
    result = nvs_get_i32(handle, Key(kind), &value);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return Default(kind);
    if (result != ESP_OK) return -1;
    return IsValidWatchLevel(kind, value) ? value : Default(kind);
}

bool SaveWatchLevel(WatchLevel kind, int value) {
    if (!IsValidWatchLevel(kind, value)) return false;
    nvs_handle_t handle = 0;
    auto result = nvs_open(Namespace(kind), NVS_READWRITE, &handle);
    if (result != ESP_OK) return false;
    result = nvs_set_i32(handle, Key(kind), value);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}
