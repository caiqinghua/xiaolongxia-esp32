#pragma once

#include "watch_time_tools.h"
#include <string>
#include <string_view>

struct WatchTimeState {
    WatchTimeCheckpoint checkpoint;
    std::string timezone = "UTC";
    bool hour24 = true;
    uint8_t alarm_alerts = 0;
};

struct WatchTimeZone {
    const char* id;
    const char* posix;
};

const WatchTimeZone* FindWatchTimeZone(std::string_view id);
const WatchTimeZone* WatchTimeZones(size_t& count);
std::string EncodeWatchTimeState(const WatchTimeState& state);
bool DecodeWatchTimeState(std::string_view json, WatchTimeState& state);
