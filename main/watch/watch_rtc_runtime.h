#pragma once

#include "watch_pcf85063.h"
#include <driver/i2c_master.h>
#include <mutex>

class WatchRtcRuntime final : private WatchRtcBus {
public:
    WatchRtcRuntime() : rtc_(*this) {}
    ~WatchRtcRuntime();
    WatchRtcRuntime(const WatchRtcRuntime&) = delete;
    WatchRtcRuntime& operator=(const WatchRtcRuntime&) = delete;
    bool Begin(i2c_master_bus_handle_t bus);
    bool ReadTime(uint64_t& unix_ms);
    bool SaveTime(uint64_t unix_ms);
    void Suspend();
    void Resume();
private:
    bool Read(uint8_t reg, uint8_t* data, size_t size) override;
    bool Write(uint8_t reg, const uint8_t* data, size_t size) override;
    std::mutex mutex_;
    i2c_master_dev_handle_t device_ = nullptr;
    bool suspended_ = false;
    WatchPcf85063 rtc_;
};
