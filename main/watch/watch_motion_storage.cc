#include "watch_motion_storage.h"

#include "system/audream_power_manager.h"
#include "esp_log.h"
#include "nvs.h"

namespace {
constexpr char kTag[] = "WatchMotion";

class StorageReservation {
public:
    explicit StorageReservation(bool exclusive_sleep) : exclusive_(exclusive_sleep),
        held_(exclusive_sleep || AudreamPowerManager::GetInstance().BeginStorageRead()) {}
    ~StorageReservation() {
        if (held_ && !exclusive_) AudreamPowerManager::GetInstance().EndStorageRead();
    }
    bool Held() const { return held_; }
private:
    bool exclusive_;
    bool held_;
};
}  // namespace

WatchMotionLoad LoadWatchActivityRecord(WatchActivityRecord& record, bool sleeping) {
    StorageReservation reservation(sleeping);
    if (!reservation.Held()) return WatchMotionLoad::kBusy;
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open("audream_watch", NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return WatchMotionLoad::kAbsent;
    if (result != ESP_OK) return WatchMotionLoad::kFailed;
    WatchActivityRecord loaded{};
    size_t size = loaded.size();
    result = nvs_get_blob(handle, "activity_v1", loaded.data(), &size);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return WatchMotionLoad::kAbsent;
    if (result != ESP_OK || size != loaded.size()) {
        ESP_LOGE(kTag, "Activity checkpoint read failed: error=%d bytes=%u", result, unsigned(size));
        return (result == ESP_OK || result == ESP_ERR_NVS_INVALID_LENGTH)
            ? WatchMotionLoad::kCorrupt : WatchMotionLoad::kFailed;
    }
    record = loaded;
    return WatchMotionLoad::kLoaded;
}

bool SaveWatchActivityRecord(const WatchActivityRecord& record, bool sleeping) {
    StorageReservation reservation(sleeping);
    if (!reservation.Held()) return false;
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open("audream_watch", NVS_READWRITE, &handle);
    if (result != ESP_OK) return false;
    result = nvs_set_blob(handle, "activity_v1", record.data(), record.size());
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) ESP_LOGE(kTag, "Activity checkpoint commit failed: error=%d", result);
    return result == ESP_OK;
}

WatchWristLoad LoadWatchWristPreferences(WatchWristPreferences& preferences) {
    StorageReservation reservation(false);
    if (!reservation.Held()) return WatchWristLoad::kFailed;
    nvs_handle_t handle = 0;
    auto result = nvs_open("audream_watch", NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) { preferences = {}; return WatchWristLoad::kAbsent; }
    if (result != ESP_OK) return WatchWristLoad::kFailed;
    WatchWristRecord record{};
    size_t size = record.size();
    result = nvs_get_blob(handle, "wrist_v1", record.data(), &size);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) { preferences = {}; return WatchWristLoad::kAbsent; }
    if (result != ESP_OK || size != record.size() || !DecodeWatchWristPreferences(record, preferences)) return WatchWristLoad::kFailed;
    return WatchWristLoad::kLoaded;
}

bool SaveWatchWristPreferences(const WatchWristPreferences& preferences) {
    const auto record = EncodeWatchWristPreferences(preferences);
    if (!record) return false;
    StorageReservation reservation(false);
    if (!reservation.Held()) return false;
    nvs_handle_t handle = 0;
    auto result = nvs_open("audream_watch", NVS_READWRITE, &handle);
    if (result != ESP_OK) return false;
    result = nvs_set_blob(handle, "wrist_v1", record->data(), record->size());
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result == ESP_OK;
}
