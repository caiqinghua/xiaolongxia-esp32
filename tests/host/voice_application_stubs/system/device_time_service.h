#pragma once
#include <cstdint>
class DeviceTimeService {
public:
    bool synchronized = true;
    uint64_t milliseconds = 1789401600000ULL;
    static DeviceTimeService& GetInstance() { static DeviceTimeService clock; return clock; }
    bool IsSynchronized() const { return synchronized; }
    uint64_t CurrentUnixTimeMs() const { return milliseconds; }
};
