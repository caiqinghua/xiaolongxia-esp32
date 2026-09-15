#include "watch_calendar.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <mutex>

namespace {
std::mutex calendar_mutex;
bool ConfigureZone(const std::string& timezone) {
    const auto* zone = FindWatchTimeZone(timezone);
    if (!zone || setenv("TZ", zone->posix, 1) != 0) { return false; }
    tzset();
    return true;
}
bool ValidUnixTime(uint64_t unix_ms) {
    return unix_ms >= 1577836800000ULL && unix_ms < 4102444800000ULL;
}
int32_t DayIdentity(const std::tm& value) {
    return (value.tm_year + 1900) * 366 + value.tm_yday;
}
}  // namespace

bool WatchLocalTime(uint64_t unix_ms, const std::string& timezone, std::tm& local) {
    std::lock_guard<std::mutex> lock(calendar_mutex);
    if (!ValidUnixTime(unix_ms) || !ConfigureZone(timezone)) { return false; }
    const time_t seconds = static_cast<time_t>(unix_ms / 1000);
    return localtime_r(&seconds, &local) != nullptr;
}

WatchCalendarMinute WatchCalendarAt(uint64_t unix_ms, const std::string& timezone) {
    std::tm local{};
    if (!WatchLocalTime(unix_ms, timezone, local)) { return {}; }
    return {true, DayIdentity(local), static_cast<uint8_t>(local.tm_wday),
        static_cast<uint8_t>(local.tm_hour), static_cast<uint8_t>(local.tm_min)};
}

std::string WatchCivilDateLabel(int32_t day) {
    const int year = day / 366;
    int remaining = day % 366;
    if (year < 2020 || year >= 2100) return {};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (remaining >= (leap ? 366 : 365)) return {};
    const int months[] = {31, leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int month = 0;
    while (month < 12 && remaining >= months[month]) remaining -= months[month++];
    if (month == 12) return {};
    char label[16];
    std::snprintf(label, sizeof(label), "%04d-%02d-%02d", year, month + 1, remaining + 1);
    return label;
}

uint64_t WatchNextAlarmDelayMs(uint64_t unix_ms, const std::string& timezone,
    const std::array<WatchAlarm, 5>& alarms) {
    std::lock_guard<std::mutex> lock(calendar_mutex);
    if (!ValidUnixTime(unix_ms) || !ConfigureZone(timezone)) { return 0; }
    const time_t seconds = static_cast<time_t>(unix_ms / 1000);
    std::tm current{};
    if (!localtime_r(&seconds, &current)) { return 0; }
    uint64_t best = std::numeric_limits<uint64_t>::max();
    for (int offset = 0; offset <= 7; ++offset) {
        auto date = current;
        date.tm_mday += offset;
        date.tm_hour = 12;
        date.tm_min = 0;
        date.tm_sec = 0;
        date.tm_isdst = -1;
        if (mktime(&date) == static_cast<time_t>(-1)) { continue; }
        for (const auto& alarm : alarms) {
            if (!alarm.enabled || DayIdentity(date) <= alarm.last_fired_day ||
                (alarm.weekdays && (alarm.weekdays & (1U << date.tm_wday)) == 0)) { continue; }
            // Test both sides of a DST fold. A nonexistent spring-forward minute
            // is skipped, while a repeated minute fires at most once per date.
            for (int dst = 0; dst <= 1; ++dst) {
                auto candidate = date;
                candidate.tm_hour = alarm.hour;
                candidate.tm_min = alarm.minute;
                candidate.tm_sec = 0;
                candidate.tm_isdst = dst;
                const time_t target = mktime(&candidate);
                if (target < 0 || candidate.tm_year != date.tm_year || candidate.tm_yday != date.tm_yday ||
                    candidate.tm_hour != alarm.hour || candidate.tm_min != alarm.minute) { continue; }
                const uint64_t target_ms = static_cast<uint64_t>(target) * 1000;
                if (target_ms > unix_ms) { best = std::min(best, target_ms - unix_ms); }
            }
        }
    }
    return best == std::numeric_limits<uint64_t>::max() ? 0 : best;
}
