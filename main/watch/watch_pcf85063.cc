#include "watch_pcf85063.h"

#include <array>

namespace {
constexpr uint64_t kEpoch2000Seconds = 946684800ULL;
constexpr uint64_t kEpoch2100Seconds = 4102444800ULL;
constexpr uint8_t kStop = 0x20;
constexpr uint8_t kMode12Hour = 0x02;
constexpr uint8_t kInvalidControl = 0xd8;
constexpr uint8_t kUtcMarker = 0xa1;
constexpr unsigned kMonthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
unsigned MonthDays(unsigned year, unsigned month) {
    return kMonthDays[month - 1] + (month == 2 && year % 4 == 0 ? 1 : 0);
}
uint8_t Bcd(unsigned value) { return static_cast<uint8_t>((value / 10) * 16 + value % 10); }
bool Decode(uint8_t raw, unsigned minimum, unsigned maximum, unsigned& value) {
    if ((raw & 15) > 9 || (raw >> 4) > 9) return false;
    value = (raw >> 4) * 10 + (raw & 15);
    return value >= minimum && value <= maximum;
}
}

bool WatchPcf85063::ReadUnixMs(uint64_t& unix_ms) {
    return ReadCalendar(unix_ms, true);
}

bool WatchPcf85063::ReadCalendar(uint64_t& unix_ms, bool require_marker) {
    // One burst includes all calendar registers, avoiding a date/time rollover
    // between separate reads. Bus operations must complete in under one second.
    std::array<uint8_t, 11> data{};
    if (!bus_.Read(0, data.data(), data.size()) || (data[0] & (kInvalidControl | kStop)) ||
        (data[4] & 0x80) || (require_marker && data[3] != kUtcMarker)) return false;
    unsigned second, minute, hour, day, month, year;
    if (!Decode(data[4], 0, 59, second) || !Decode(data[5], 0, 59, minute) ||
        !Decode(data[7], 1, 31, day) || data[8] > 6 ||
        !Decode(data[9], 1, 12, month) || !Decode(data[10], 0, 99, year)) return false;
    if (data[0] & kMode12Hour) {
        if ((data[6] & 0xc0) || !Decode(data[6] & 0x1f, 1, 12, hour)) return false;
        hour = hour % 12 + (data[6] & 0x20 ? 12 : 0);
    } else if (!Decode(data[6], 0, 23, hour)) return false;
    year += 2000;
    if (day > MonthDays(year, month)) return false;
    uint64_t days = 0;
    for (unsigned current = 2000; current < year; ++current) days += current % 4 == 0 ? 366 : 365;
    for (unsigned current = 1; current < month; ++current) days += MonthDays(year, current);
    days += day - 1;
    if ((days + 6) % 7 != data[8]) return false;
    unix_ms = (kEpoch2000Seconds + days * 86400 + hour * 3600 + minute * 60 + second) * 1000;
    return true;
}

bool WatchPcf85063::WriteUnixMs(uint64_t unix_ms) {
    const uint64_t seconds = unix_ms / 1000;
    if (seconds < kEpoch2000Seconds || seconds >= kEpoch2100Seconds) return false;
    const uint64_t since2000 = seconds - kEpoch2000Seconds;
    unsigned days = static_cast<unsigned>(since2000 / 86400);
    const uint8_t weekday = static_cast<uint8_t>((days + 6) % 7);
    unsigned year = 2000, month = 1;
    while (days >= (year % 4 == 0 ? 366U : 365U)) {
        days -= year % 4 == 0 ? 366 : 365;
        ++year;
    }
    while (days >= MonthDays(year, month)) days -= MonthDays(year, month++);
    const std::array<uint8_t, 7> calendar = {Bcd(seconds % 60), Bcd((seconds / 60) % 60),
        Bcd((seconds / 3600) % 24), Bcd(days + 1), weekday, Bcd(month), Bcd(year - 2000)};
    uint8_t control = 0;
    if (!bus_.Read(0, &control, 1) || (control & kInvalidControl)) return false;
    const uint8_t invalid_marker = 0;
    if (!bus_.Write(3, &invalid_marker, 1)) return false;
    control &= static_cast<uint8_t>(~(kStop | kMode12Hour));
    const uint8_t stopped = control | kStop;
    if (!bus_.Write(0, &stopped, 1)) return false;
    // On a partial write leave STOP set: an interrupted update is invalid, not
    // a plausible-looking timestamp for the next cold boot.
    if (!bus_.Write(4, calendar.data(), calendar.size()) || !bus_.Write(0, &control, 1)) return false;
    uint64_t verified = 0;
    if (!ReadCalendar(verified, false) || verified < seconds * 1000 || verified > seconds * 1000 + 1000) return false;
    // Unmarked legacy RTC values may contain local time. Mark UTC only after
    // successful programming and readback, never by merely observing a date.
    if (!bus_.Write(3, &kUtcMarker, 1)) return false;
    return ReadUnixMs(verified) && verified >= seconds * 1000 && verified <= seconds * 1000 + 1000;
}
