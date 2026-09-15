#include "watch/watch_pcf85063.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <ctime>
#include <iostream>

struct Bus : WatchRtcBus {
    std::array<uint8_t, 18> registers{};
    int calls = 0, fail_at = 0;
    bool corrupt = false;
    bool Read(uint8_t reg, uint8_t* data, size_t size) override {
        assert(reg + size <= registers.size());
        if (++calls == fail_at) return false;
        std::copy_n(registers.data() + reg, size, data);
        return true;
    }
    bool Write(uint8_t reg, const uint8_t* data, size_t size) override {
        assert(reg + size <= registers.size());
        if (++calls == fail_at) {
            if (size > 1) std::copy_n(data, size / 2, registers.data() + reg);
            return false;
        }
        if (reg == 4) assert(size == 7);
        std::copy_n(data, size, registers.data() + reg);
        if (corrupt && reg == 4) registers[10] = 0x99;
        return true;
    }
};

int main() {
    Bus bus;
    WatchPcf85063 rtc(bus);
    uint64_t output = 123;
    assert(!rtc.ReadUnixMs(output) && output == 123);
    constexpr uint64_t base = 1709164800000ULL; // 2024-02-29 00:00:00 UTC.
    assert(rtc.WriteUnixMs(base));
    assert(rtc.ReadUnixMs(output) && output == base);
    const auto valid = bus.registers;
    for (const auto& invalid : std::array<std::pair<unsigned, uint8_t>, 13>{{
        {0, 0x20}, {0, 0x80}, {0, 0x08}, {3, 0}, {4, 0x80}, {4, 0x1a},
        {5, 0x80}, {6, 0x24}, {7, 0x30}, {8, 7}, {9, 0}, {9, 0x13}, {10, 0x23}}}) {
        bus.registers = valid; bus.registers[invalid.first] = invalid.second;
        output = 123;
        assert(!rtc.ReadUnixMs(output) && output == 123);
    }
    bus.registers = valid;
    bus.registers[3] = 0; // Legacy firmware may have stored local time.
    assert(!rtc.ReadUnixMs(output));
    bus.registers = valid; bus.registers[0] = 2; bus.registers[6] = 0x12;
    assert(rtc.ReadUnixMs(output) && output == base);
    bus.registers[6] = 0x32;
    assert(rtc.ReadUnixMs(output) && output == base + 12 * 3600000ULL);
    bus.registers[6] = 0;
    assert(!rtc.ReadUnixMs(output));
    for (int failure = 1; failure <= 8; ++failure) {
        bus.registers = valid; bus.calls = 0; bus.fail_at = failure;
        assert(!rtc.WriteUnixMs(base + 3600000));
        bus.fail_at = 0;
        if (failure >= 3 && failure <= 7) assert(!rtc.ReadUnixMs(output));
    }
    bus.registers = valid; bus.corrupt = true;
    assert(!rtc.WriteUnixMs(base));
    assert(!rtc.ReadUnixMs(output));
    bus.corrupt = false; bus.registers = valid;
    const int calls = bus.calls;
    assert(!rtc.WriteUnixMs(946684799999ULL));
    assert(!rtc.WriteUnixMs(4102444800000ULL));
    assert(!rtc.WriteUnixMs(UINT64_MAX));
    assert(bus.calls == calls);
    // Compare every supported civil day with the host's independent UTC calendar.
    for (uint64_t seconds = 946684800ULL; seconds < 4102444800ULL; seconds += 86400) {
        const uint64_t value = (seconds + 45296) * 1000 + 789;
        assert(rtc.WriteUnixMs(value));
        assert(rtc.ReadUnixMs(output) && output == value - 789);
        const time_t timestamp = static_cast<time_t>(seconds + 45296);
        const auto calendar = *std::gmtime(&timestamp);
        const auto decode = [](uint8_t raw) { return (raw >> 4) * 10 + (raw & 15); };
        assert(decode(bus.registers[10]) == calendar.tm_year - 100);
        assert(decode(bus.registers[9]) == calendar.tm_mon + 1);
        assert(decode(bus.registers[7]) == calendar.tm_mday);
        assert(bus.registers[8] == calendar.tm_wday);
        assert(decode(bus.registers[6]) == calendar.tm_hour);
        assert(decode(bus.registers[5]) == calendar.tm_min);
        assert(decode(bus.registers[4]) == calendar.tm_sec);
    }
    std::cout << "PASS: PCF85063 UTC, all civil dates 2000-2099, mode/OS/BCD validation and interrupted writes\n";
}
