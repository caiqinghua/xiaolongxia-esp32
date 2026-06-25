#pragma once

#include <cstdint>
#include <mutex>
#include <string>

class DeviceTimeService {
public:
    static DeviceTimeService& GetInstance();

    void Initialize();
    void SyncUnixTimeMs(uint64_t unix_time_ms);
    bool IsSynchronized() const;
    uint64_t CurrentUnixTimeMs() const;
    std::string RtcEpochID() const;

private:
    DeviceTimeService() = default;

    bool PersistLocked() const;
    void EnsureRtcEpochIDLocked();

    mutable std::mutex mutex_;
    std::string rtc_epoch_id_;
    bool synchronized_ = false;
    uint64_t synced_unix_time_ms_ = 0;
    uint64_t synced_rtc_time_us_ = 0;
};
