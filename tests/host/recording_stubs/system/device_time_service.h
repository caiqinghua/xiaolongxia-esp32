#pragma once
#include "esp_timer.h"
class DeviceTimeService {
public:
    static DeviceTimeService& GetInstance() { static DeviceTimeService value; return value; }
    uint64_t RtcEpochSeq() const { return 1; }
    uint64_t CurrentUnixTimeMs() const { return 1700000000000ULL + test_time_us / 1000; }
    bool IsSynchronized() const { return true; }
};
