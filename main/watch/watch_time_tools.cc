#include "watch_time_tools.h"

#include <algorithm>
#include <limits>

namespace {
uint64_t Elapsed(uint64_t now_ms, uint64_t started_ms) {
    return now_ms >= started_ms ? now_ms - started_ms : 0;
}
uint64_t AddSaturated(uint64_t a, uint64_t b) {
    return b > std::numeric_limits<uint64_t>::max() - a
        ? std::numeric_limits<uint64_t>::max() : a + b;
}
bool ValidAlarm(const WatchAlarm& alarm) {
    return alarm.hour < 24 && alarm.minute < 60 && alarm.weekdays < 128 && alarm.last_fired_day >= -1;
}
}  // namespace

void WatchTimeTools::StartStopwatch(uint64_t now_ms) {
    if (!stopwatch_running_) {
        stopwatch_started_ms_ = now_ms;
        stopwatch_running_ = true;
    }
}

void WatchTimeTools::PauseStopwatch(uint64_t now_ms) {
    stopwatch_accumulated_ms_ = StopwatchElapsedMs(now_ms);
    stopwatch_running_ = false;
}

void WatchTimeTools::ResetStopwatch(uint64_t now_ms) {
    stopwatch_accumulated_ms_ = 0;
    stopwatch_started_ms_ = now_ms;
    stopwatch_running_ = false;
    laps_.clear();
}

uint64_t WatchTimeTools::StopwatchElapsedMs(uint64_t now_ms) const {
    return AddSaturated(stopwatch_accumulated_ms_,
        stopwatch_running_ ? Elapsed(now_ms, stopwatch_started_ms_) : 0);
}

bool WatchTimeTools::AddLap(uint64_t now_ms) {
    if (!stopwatch_running_ || laps_.size() >= kMaximumLaps) {
        return false;
    }
    laps_.push_back(StopwatchElapsedMs(now_ms));
    return true;
}

bool WatchTimeTools::StartTimer(uint64_t duration_ms, uint64_t now_ms) {
    if (duration_ms == 0 || duration_ms > kMaximumTimerMs) {
        return false;
    }
    timer_remaining_ms_ = duration_ms;
    timer_started_ms_ = now_ms;
    timer_running_ = true;
    timer_configured_ = true;
    timer_alert_ = false;
    return true;
}

uint64_t WatchTimeTools::TimerRemainingMs(uint64_t now_ms) const {
    const uint64_t elapsed = timer_running_ ? Elapsed(now_ms, timer_started_ms_) : 0;
    return elapsed >= timer_remaining_ms_ ? 0 : timer_remaining_ms_ - elapsed;
}

void WatchTimeTools::PauseTimer(uint64_t now_ms) {
    if (timer_running_) {
        timer_remaining_ms_ = TimerRemainingMs(now_ms);
        timer_running_ = false;
        if (timer_remaining_ms_ == 0) {
            timer_alert_ = true;
        }
    }
}

bool WatchTimeTools::ResumeTimer(uint64_t now_ms) {
    if (!timer_configured_ || timer_running_ || timer_remaining_ms_ == 0) {
        return false;
    }
    timer_started_ms_ = now_ms;
    timer_running_ = true;
    return true;
}

void WatchTimeTools::CancelTimer() {
    timer_remaining_ms_ = 0;
    timer_running_ = false;
    timer_configured_ = false;
    timer_alert_ = false;
}

bool WatchTimeTools::SetAlarm(size_t index, const WatchAlarm& alarm) {
    if (index >= alarms_.size() || !ValidAlarm(alarm)) {
        return false;
    }
    alarms_[index] = alarm;
    alarms_[index].last_fired_day = -1;
    return true;
}

WatchTimeEvents WatchTimeTools::Poll(uint64_t now_ms, const WatchCalendarMinute& calendar) {
    WatchTimeEvents events;
    if (timer_running_ && TimerRemainingMs(now_ms) == 0) {
        timer_remaining_ms_ = 0;
        timer_running_ = false;
        timer_alert_ = true;
        events.timer_expired = true;
    }
    if (!calendar.valid || calendar.weekday > 6 || calendar.hour > 23 || calendar.minute > 59 || calendar.day < 0) {
        return events;
    }
    for (size_t index = 0; index < alarms_.size(); ++index) {
        auto& alarm = alarms_[index];
        if (!alarm.enabled || alarm.hour != calendar.hour || alarm.minute != calendar.minute ||
            calendar.day <= alarm.last_fired_day ||
            (alarm.weekdays != 0 && (alarm.weekdays & (1U << calendar.weekday)) == 0)) {
            continue;
        }
        alarm.last_fired_day = calendar.day;
        if (alarm.weekdays == 0) {
            alarm.enabled = false;
        }
        events.alarms.push_back(index);
    }
    return events;
}

WatchTimeCheckpoint WatchTimeTools::Checkpoint(uint64_t now_ms, uint64_t rtc_epoch) const {
    WatchTimeCheckpoint result;
    result.rtc_epoch = rtc_epoch;
    result.saved_rtc_ms = now_ms;
    result.stopwatch_elapsed_ms = StopwatchElapsedMs(now_ms);
    result.stopwatch_running = stopwatch_running_;
    result.timer_remaining_ms = TimerRemainingMs(now_ms);
    result.timer_running = timer_running_;
    result.timer_configured = timer_configured_;
    result.timer_alert = timer_alert_;
    result.alarms = alarms_;
    result.laps = laps_;
    return result;
}

bool WatchTimeTools::Restore(const WatchTimeCheckpoint& checkpoint, uint64_t now_ms, uint64_t rtc_epoch) {
    if (checkpoint.version != 1 || checkpoint.timer_remaining_ms > kMaximumTimerMs ||
        (!checkpoint.timer_configured && (checkpoint.timer_running || checkpoint.timer_alert || checkpoint.timer_remaining_ms)) ||
        (checkpoint.timer_alert && checkpoint.timer_running) || checkpoint.laps.size() > kMaximumLaps) {
        return false;
    }
    uint64_t previous_lap = 0;
    for (const uint64_t lap : checkpoint.laps) {
        if (lap < previous_lap || lap > checkpoint.stopwatch_elapsed_ms) { return false; }
        previous_lap = lap;
    }
    for (const auto& alarm : checkpoint.alarms) {
        if (!ValidAlarm(alarm)) {
            return false;
        }
    }
    const bool same_clock = rtc_epoch != 0 && rtc_epoch == checkpoint.rtc_epoch && now_ms >= checkpoint.saved_rtc_ms;
    const uint64_t elapsed = same_clock ? now_ms - checkpoint.saved_rtc_ms : 0;
    stopwatch_accumulated_ms_ = AddSaturated(checkpoint.stopwatch_elapsed_ms,
        same_clock && checkpoint.stopwatch_running ? elapsed : 0);
    stopwatch_started_ms_ = now_ms;
    stopwatch_running_ = same_clock && checkpoint.stopwatch_running;
    timer_remaining_ms_ = checkpoint.timer_remaining_ms;
    if (same_clock && checkpoint.timer_running) {
        timer_remaining_ms_ -= std::min(timer_remaining_ms_, elapsed);
    }
    timer_started_ms_ = now_ms;
    timer_running_ = same_clock && checkpoint.timer_running;
    timer_configured_ = checkpoint.timer_configured;
    timer_alert_ = checkpoint.timer_alert;
    interrupted_by_reset_ = !same_clock && (checkpoint.stopwatch_running || checkpoint.timer_running);
    alarms_ = checkpoint.alarms;
    laps_ = checkpoint.laps;
    return true;
}
