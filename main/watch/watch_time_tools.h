#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

struct WatchAlarm {
    uint8_t hour = 7;
    uint8_t minute = 0;
    uint8_t weekdays = 0;  // Bit zero is Sunday; zero means the next occurrence only.
    bool enabled = false;
    int32_t last_fired_day = -1;
};

struct WatchCalendarMinute {
    bool valid = false;
    int32_t day = 0;  // Stable increasing local civil-date identity, not UTC days.
    uint8_t weekday = 0;
    uint8_t hour = 0;
    uint8_t minute = 0;
};

struct WatchTimeCheckpoint {
    uint32_t version = 1;
    uint64_t rtc_epoch = 0;
    uint64_t saved_rtc_ms = 0;
    uint64_t stopwatch_elapsed_ms = 0;
    uint64_t timer_remaining_ms = 0;
    bool stopwatch_running = false;
    bool timer_running = false;
    bool timer_configured = false;
    bool timer_alert = false;
    std::array<WatchAlarm, 5> alarms{};
    std::vector<uint64_t> laps;
};

struct WatchTimeEvents {
    bool timer_expired = false;
    std::vector<size_t> alarms;
};

// The owning service serializes calls. Durations use the RTC monotonic clock;
// only calendar alarms use localized wall-clock values.
class WatchTimeTools {
public:
    static constexpr uint64_t kMaximumTimerMs = 7ULL * 24 * 60 * 60 * 1000;
    static constexpr size_t kMaximumLaps = 20;

    void StartStopwatch(uint64_t now_ms);
    void PauseStopwatch(uint64_t now_ms);
    void ResetStopwatch(uint64_t now_ms);
    bool AddLap(uint64_t now_ms);
    uint64_t StopwatchElapsedMs(uint64_t now_ms) const;
    bool StopwatchRunning() const { return stopwatch_running_; }
    const std::vector<uint64_t>& Laps() const { return laps_; }

    bool StartTimer(uint64_t duration_ms, uint64_t now_ms);
    void PauseTimer(uint64_t now_ms);
    bool ResumeTimer(uint64_t now_ms);
    void CancelTimer();
    void DismissTimerAlert() { timer_alert_ = false; }
    uint64_t TimerRemainingMs(uint64_t now_ms) const;
    bool TimerRunning() const { return timer_running_; }
    bool TimerConfigured() const { return timer_configured_; }
    bool TimerAlert() const { return timer_alert_; }
    bool InterruptedByReset() const { return interrupted_by_reset_; }

    bool SetAlarm(size_t index, const WatchAlarm& alarm);
    const std::array<WatchAlarm, 5>& Alarms() const { return alarms_; }
    WatchTimeEvents Poll(uint64_t now_ms, const WatchCalendarMinute& calendar);
    WatchTimeCheckpoint Checkpoint(uint64_t now_ms, uint64_t rtc_epoch) const;
    bool Restore(const WatchTimeCheckpoint& checkpoint, uint64_t now_ms, uint64_t rtc_epoch);

private:
    uint64_t stopwatch_started_ms_ = 0;
    uint64_t stopwatch_accumulated_ms_ = 0;
    uint64_t timer_started_ms_ = 0;
    uint64_t timer_remaining_ms_ = 0;
    bool stopwatch_running_ = false;
    bool timer_running_ = false;
    bool timer_configured_ = false;
    bool timer_alert_ = false;
    bool interrupted_by_reset_ = false;
    std::vector<uint64_t> laps_;
    std::array<WatchAlarm, 5> alarms_{};
};
