#pragma once

#include <cstdint>
#include <mutex>

class DeviceTimeService {
public:
    static DeviceTimeService& GetInstance();

    void Initialize();
    void SyncUnixTimeMs(uint64_t unix_time_ms);
    bool IsSynchronized() const;
    uint64_t CurrentUnixTimeMs() const;
    uint64_t RtcEpochSeq() const;

private:
    DeviceTimeService() = default;

    bool PersistLocked() const;
    bool PersistRtcEpochSeqLocked() const;
    void EnsureRtcEpochSeqLocked();

    mutable std::mutex mutex_;
    uint64_t rtc_epoch_seq_ = 0;
    bool synchronized_ = false;
    uint64_t synced_unix_time_ms_ = 0;
    uint64_t synced_rtc_time_us_ = 0;
};
