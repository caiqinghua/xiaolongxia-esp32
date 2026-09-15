#pragma once

#include "watch_time_service.h"
#include <string>

enum class WatchTimeTool { kStatus, kStartTimer, kCancelTimer };
struct WatchAlarmToolRequest {
    int index = 0;
    int hour = 0;
    int minute = 0;
    int weekdays = 0;
    bool enabled = false;
    std::string expected_state;
};
std::string ExecuteWatchTimeTool(WatchTimeService& service, WatchTimeTool tool,
    const WatchClockReading& clock, bool allowed, int seconds = 0);
std::string ExecuteWatchAlarmTool(WatchTimeService& service, const WatchAlarmToolRequest& request,
    const WatchClockReading& clock, bool allowed);
