#pragma once

#include "watch_time_state.h"
#include <ctime>

// These calls serialize the process-wide libc timezone setting. UTC remains the
// storage and synchronization format; only watch calendar conversion uses TZ.
bool WatchLocalTime(uint64_t unix_ms, const std::string& timezone, std::tm& local);
WatchCalendarMinute WatchCalendarAt(uint64_t unix_ms, const std::string& timezone);
// Decode the stored civil-date identity, not a Unix day or a timezone conversion.
std::string WatchCivilDateLabel(int32_t day);
uint64_t WatchNextAlarmDelayMs(uint64_t unix_ms, const std::string& timezone,
    const std::array<WatchAlarm, 5>& alarms);
