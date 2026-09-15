#pragma once
#include "wifi_station.h"
struct TestRecordingState { bool starting = false; };
class Board {
public:
    static Board& GetInstance() { static Board board; return board; }
    bool credentials = true, provisioning = false, starting = false;
    bool start_succeeds = true, stop_succeeds = true;
    int battery = 80;
    bool charging = false, battery_valid = true;
    bool GetBatteryLevel(int& level, bool& charge, bool& discharge) {
        level = battery; charge = charging; discharge = !charging; return battery_valid;
    }
    unsigned station_starts = 0, station_stops = 0;
    TestRecordingState GetWatchRecordingControlStatus() const { return {starting}; }
    bool HasWatchWifiCredentials() const { return credentials; }
    bool IsWatchWifiProvisioning() const { return provisioning; }
    bool RestoreWatchStation() { ++station_starts; return start_succeeds; }
    bool SuspendIdleWatchStation() { return !provisioning && ApplyWatchWifiEnabled(false); }
    bool ApplyWatchWifiEnabled(bool enabled) {
        if (enabled) return RestoreWatchStation();
        ++station_stops;
        if (stop_succeeds) WifiStation::GetInstance().connected = false;
        return stop_succeeds;
    }
};
