#include "watch_time_runtime.h"

#include "system/device_time_service.h"
#include "esp_log.h"
#include "esp_rtc_time.h"
#include "nvs.h"
#include <vector>

namespace {
constexpr char kTag[] = "WatchTime";

class NvsWatchTimeStorage final : public WatchTimeStorage {
public:
    WatchStateLoad Load(std::string& value) override {
        nvs_handle_t handle = 0;
        auto result = nvs_open("audream_watch", NVS_READONLY, &handle);
        if (result == ESP_ERR_NVS_NOT_FOUND) { return WatchStateLoad::kAbsent; }
        if (result != ESP_OK) { return WatchStateLoad::kFailed; }
        size_t size = 0;
        result = nvs_get_str(handle, "time_state", nullptr, &size);
        if (result == ESP_ERR_NVS_NOT_FOUND) {
            nvs_close(handle);
            return WatchStateLoad::kAbsent;
        }
        if (result != ESP_OK || size < 2 || size > 4097) {
            nvs_close(handle);
            return WatchStateLoad::kFailed;
        }
        std::vector<char> buffer(size);
        result = nvs_get_str(handle, "time_state", buffer.data(), &size);
        nvs_close(handle);
        if (result != ESP_OK || size < 2 || size > buffer.size() || buffer[size - 1] != '\0') {
            return WatchStateLoad::kFailed;
        }
        value.assign(buffer.data(), size - 1);
        return WatchStateLoad::kLoaded;
    }

    bool Save(const std::string& value) override {
        if (value.empty() || value.size() > 4096) { return false; }
        nvs_handle_t handle = 0;
        auto result = nvs_open("audream_watch", NVS_READWRITE, &handle);
        if (result != ESP_OK) { return false; }
        result = nvs_set_str(handle, "time_state", value.c_str());
        if (result == ESP_OK) { result = nvs_commit(handle); }
        nvs_close(handle);
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "Failed to commit watch time state: %d", static_cast<int>(result));
        }
        return result == ESP_OK;
    }
};
}  // namespace

WatchTimeService& GetWatchTimeService() {
    static NvsWatchTimeStorage storage;
    static WatchTimeService service(storage);
    return service;
}

WatchClockReading ReadWatchClock() {
    auto& time = DeviceTimeService::GetInstance();
    return {esp_rtc_get_time_us() / 1000, time.RtcEpochSeq(), time.CurrentUnixTimeMs()};
}
