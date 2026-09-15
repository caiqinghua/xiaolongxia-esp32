#pragma once
#include <cstdint>
class DeviceTimeService {
public:
    static DeviceTimeService& GetInstance() { static DeviceTimeService instance; return instance; }
    uint64_t RtcEpochSeq() const { return 7; }
    uint64_t CurrentUnixTimeMs() const { return 1789257601000ULL; }
};
