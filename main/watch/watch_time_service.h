#pragma once

#include "watch_time_state.h"
#include <functional>
#include <mutex>

enum class WatchStateLoad { kAbsent, kLoaded, kFailed };

class WatchTimeStorage {
public:
    virtual ~WatchTimeStorage() = default;
    virtual WatchStateLoad Load(std::string& value) = 0;
    virtual bool Save(const std::string& value) = 0;
};

struct WatchClockReading {
    uint64_t rtc_ms = 0;
    uint64_t rtc_epoch = 0;
    uint64_t unix_ms = 0;
};

enum class WatchTimeCommand {
    kStartStopwatch, kPauseStopwatch, kResetStopwatch, kAddLap,
    kStartTimer, kPauseTimer, kResumeTimer, kCancelTimer, kDismissAlerts,
};

struct WatchTimeSnapshot {
    WatchTimeState state;
    std::vector<uint64_t> laps;
    bool initialized = false;
    bool storage_error = false;
    bool interrupted = false;
    bool sleep_prepared = false;
};

std::string WatchAlarmStateToken(const WatchTimeState& state, size_t index);

class WatchTimeService {
public:
    explicit WatchTimeService(WatchTimeStorage& storage) : storage_(storage) {}
    bool Initialize(const WatchClockReading& clock);
    bool Execute(WatchTimeCommand command, const WatchClockReading& clock, uint64_t duration_ms = 0);
    bool SetAlarm(size_t index, const WatchAlarm& alarm, const WatchClockReading& clock);
    bool SetAlarmChecked(size_t index, const WatchAlarm& alarm, const std::string& expected_state,
        const WatchClockReading& clock);
    bool SetPreferences(const std::string& timezone, bool hour24, const WatchClockReading& clock);
    WatchTimeEvents Poll(const WatchClockReading& clock);
    WatchTimeSnapshot Snapshot(const WatchClockReading& clock) const;
    bool PrepareForSleep(const WatchClockReading& clock, uint64_t& wake_delay_ms);
    bool SleepWakeDelay(const WatchClockReading& clock, uint64_t& wake_delay_ms) const;
    void CancelSleep();

private:
    mutable std::mutex mutex_;
    WatchTimeStorage& storage_;
    WatchTimeTools tools_;
    std::string timezone_ = "UTC";
    bool hour24_ = true;
    uint8_t alarm_alerts_ = 0;
    bool initialized_ = false;
    bool storage_error_ = false;
    bool load_failed_ = false;
    bool sleep_prepared_ = false;
    WatchClockReading sleep_clock_;
    uint64_t sleep_wake_delay_ms_ = 0;

    WatchTimeState StateLocked(const WatchClockReading& clock) const;
    bool SaveLocked(const WatchClockReading& clock);
    bool ChangeLocked(const WatchClockReading& clock, const std::function<bool()>& change);
    WatchTimeEvents PollLocked(const WatchClockReading& clock);
};
