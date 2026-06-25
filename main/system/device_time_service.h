#pragma once

#include <cstdint>
#include <mutex>

class DeviceTimeService {
public:
    static DeviceTimeService& GetInstance();

    void SyncUnixTimeMs(uint64_t unix_time_ms);
    bool IsSynchronized() const;
    uint64_t CurrentUnixTimeMs() const;

private:
    DeviceTimeService() = default;

    mutable std::mutex mutex_;
    bool synchronized_ = false;
    uint64_t synced_unix_time_ms_ = 0;
    int64_t synced_esp_time_us_ = 0;
};
