#include "system/device_time_service.h"

#include "esp_log.h"
#include "esp_timer.h"

namespace {

constexpr char kTag[] = "DeviceTime";

}  // namespace

DeviceTimeService& DeviceTimeService::GetInstance() {
    static DeviceTimeService instance;
    return instance;
}

void DeviceTimeService::SyncUnixTimeMs(uint64_t unix_time_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    synced_unix_time_ms_ = unix_time_ms;
    synced_esp_time_us_ = esp_timer_get_time();
    synchronized_ = true;
    ESP_LOGI(kTag, "Device time synchronized");
}

bool DeviceTimeService::IsSynchronized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return synchronized_;
}

uint64_t DeviceTimeService::CurrentUnixTimeMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!synchronized_) {
        return 0;
    }
    const int64_t elapsed_us = esp_timer_get_time() - synced_esp_time_us_;
    if (elapsed_us <= 0) {
        return synced_unix_time_ms_;
    }
    return synced_unix_time_ms_ + static_cast<uint64_t>(elapsed_us / 1000);
}
