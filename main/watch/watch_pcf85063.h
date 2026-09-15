#pragma once

#include <cstddef>
#include <cstdint>

class WatchRtcBus {
public:
    virtual ~WatchRtcBus() = default;
    virtual bool Read(uint8_t reg, uint8_t* data, size_t size) = 0;
    virtual bool Write(uint8_t reg, const uint8_t* data, size_t size) = 0;
};

// The owning runtime serializes access. RTC storage is UTC, never local time.
class WatchPcf85063 {
public:
    static constexpr uint8_t kAddress = 0x51;
    explicit WatchPcf85063(WatchRtcBus& bus) : bus_(bus) {}
    bool ReadUnixMs(uint64_t& unix_ms);
    bool WriteUnixMs(uint64_t unix_ms);
private:
    bool ReadCalendar(uint64_t& unix_ms, bool require_marker);
    WatchRtcBus& bus_;
};
