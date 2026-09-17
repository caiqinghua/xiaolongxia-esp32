#include "watch_face_settings.h"
#include "nvs.h"

namespace {
constexpr const char* kNamespace = "audream_watch";
constexpr const char* kKey = "face_style";
}

bool IsValidWatchFaceStyle(WatchFaceStyle style) {
    return style == WatchFaceStyle::kClassic || style == WatchFaceStyle::kMinimal ||
        style == WatchFaceStyle::kActivity;
}

WatchFaceStyle ReadWatchFaceStyle() {
    nvs_handle_t handle = 0;
    auto result = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return WatchFaceStyle::kClassic;
    if (result != ESP_OK) return WatchFaceStyle::kClassic;
    int32_t value = 0;
    result = nvs_get_i32(handle, kKey, &value);
    nvs_close(handle);
    const auto style = static_cast<WatchFaceStyle>(value);
    return result == ESP_OK && IsValidWatchFaceStyle(style) ? style : WatchFaceStyle::kClassic;
}

bool SaveWatchFaceStyle(WatchFaceStyle style) {
    if (!IsValidWatchFaceStyle(style)) return false;
    nvs_handle_t handle = 0;
    auto result = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return false;
    result = nvs_set_i32(handle, kKey, static_cast<int32_t>(style));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}
