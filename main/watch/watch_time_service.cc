#include "watch_time_service.h"
#include "watch_calendar.h"
#include <algorithm>
#include <limits>

std::string WatchAlarmStateToken(const WatchTimeState& state, size_t index) {
    if (index >= state.checkpoint.alarms.size()) return {};
    const auto& alarm = state.checkpoint.alarms[index];
    return "v1:" + std::to_string(state.checkpoint.rtc_epoch) + ":" + state.timezone + ":" +
        std::to_string(index) + ":" + std::to_string(alarm.hour) + ":" +
        std::to_string(alarm.minute) + ":" + std::to_string(alarm.weekdays) + ":" +
        std::to_string(alarm.enabled) + ":" + std::to_string(alarm.last_fired_day);
}

WatchTimeState WatchTimeService::StateLocked(const WatchClockReading& clock) const {
    return {tools_.Checkpoint(clock.rtc_ms, clock.rtc_epoch), timezone_, hour24_, alarm_alerts_};
}

bool WatchTimeService::Initialize(const WatchClockReading& clock) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) { return !load_failed_; }
    std::string json;
    const auto result = storage_.Load(json);
    if (result == WatchStateLoad::kFailed) {
        load_failed_ = storage_error_ = true;
        return false;
    }
    if (result == WatchStateLoad::kLoaded) {
        WatchTimeState state;
        if (!DecodeWatchTimeState(json, state) || !tools_.Restore(state.checkpoint, clock.rtc_ms, clock.rtc_epoch)) {
            load_failed_ = storage_error_ = true;
            return false;
        }
        timezone_ = state.timezone;
        hour24_ = state.hour24;
        alarm_alerts_ = state.alarm_alerts;
    }
    load_failed_ = storage_error_ = false;
    initialized_ = true;
    return true;
}

bool WatchTimeService::SaveLocked(const WatchClockReading& clock) {
    const auto json = EncodeWatchTimeState(StateLocked(clock));
    storage_error_ = json.empty() || !storage_.Save(json);
    return !storage_error_;
}

bool WatchTimeService::ChangeLocked(const WatchClockReading& clock, const std::function<bool()>& change) {
    if (!initialized_ || load_failed_ || sleep_prepared_) { return false; }
    const auto old_tools = tools_;
    const auto old_timezone = timezone_;
    const auto old_hour24 = hour24_;
    const auto old_alerts = alarm_alerts_;
    if (change() && SaveLocked(clock)) { return true; }
    tools_ = old_tools;
    timezone_ = old_timezone;
    hour24_ = old_hour24;
    alarm_alerts_ = old_alerts;
    return false;
}

bool WatchTimeService::Execute(WatchTimeCommand command, const WatchClockReading& clock, uint64_t duration_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ChangeLocked(clock, [&]() {
        switch (command) {
            case WatchTimeCommand::kStartStopwatch: tools_.StartStopwatch(clock.rtc_ms); break;
            case WatchTimeCommand::kPauseStopwatch: tools_.PauseStopwatch(clock.rtc_ms); break;
            case WatchTimeCommand::kResetStopwatch: tools_.ResetStopwatch(clock.rtc_ms); break;
            case WatchTimeCommand::kAddLap: return tools_.AddLap(clock.rtc_ms);
            case WatchTimeCommand::kStartTimer: return tools_.StartTimer(duration_ms, clock.rtc_ms);
            case WatchTimeCommand::kPauseTimer: tools_.PauseTimer(clock.rtc_ms); break;
            case WatchTimeCommand::kResumeTimer: return tools_.ResumeTimer(clock.rtc_ms);
            case WatchTimeCommand::kCancelTimer: tools_.CancelTimer(); break;
            case WatchTimeCommand::kDismissAlerts:
                tools_.DismissTimerAlert();
                alarm_alerts_ = 0;
                break;
            default: return false;
        }
        return true;
    });
}

bool WatchTimeService::SetAlarm(size_t index, const WatchAlarm& alarm, const WatchClockReading& clock) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ChangeLocked(clock, [&]() { return tools_.SetAlarm(index, alarm); });
}

bool WatchTimeService::SetAlarmChecked(size_t index, const WatchAlarm& alarm,
    const std::string& expected_state, const WatchClockReading& clock) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= tools_.Alarms().size() || expected_state.empty() ||
        expected_state != WatchAlarmStateToken(StateLocked(clock), index) ||
        (alarm.enabled && !WatchCalendarAt(clock.unix_ms, timezone_).valid)) return false;
    return ChangeLocked(clock, [&]() {
        const auto& current = tools_.Alarms()[index];
        // An identical update must not reset the daily duplicate-fire guard.
        if (current.hour == alarm.hour && current.minute == alarm.minute &&
            current.weekdays == alarm.weekdays && current.enabled == alarm.enabled) return true;
        return tools_.SetAlarm(index, alarm);
    });
}

bool WatchTimeService::SetPreferences(const std::string& timezone, bool hour24, const WatchClockReading& clock) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ChangeLocked(clock, [&]() {
        if (!FindWatchTimeZone(timezone)) { return false; }
        timezone_ = timezone;
        hour24_ = hour24;
        return true;
    });
}

WatchTimeEvents WatchTimeService::PollLocked(const WatchClockReading& clock) {
    if (!initialized_) { return {}; }
    auto events = tools_.Poll(clock.rtc_ms, WatchCalendarAt(clock.unix_ms, timezone_));
    for (const auto index : events.alarms) { alarm_alerts_ |= static_cast<uint8_t>(1U << index); }
    if (events.timer_expired || !events.alarms.empty()) {
        // Keep expiry visible even if persistence fails; do not re-ring each tick.
        SaveLocked(clock);
    }
    return events;
}

WatchTimeEvents WatchTimeService::Poll(const WatchClockReading& clock) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sleep_prepared_) { return {}; }
    return PollLocked(clock);
}

WatchTimeSnapshot WatchTimeService::Snapshot(const WatchClockReading& clock) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {StateLocked(clock), tools_.Laps(), initialized_, storage_error_, tools_.InterruptedByReset(), sleep_prepared_};
}

bool WatchTimeService::PrepareForSleep(const WatchClockReading& clock, uint64_t& wake_delay_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    wake_delay_ms = 0;
    if (!initialized_ || load_failed_ || sleep_prepared_ || clock.rtc_epoch == 0) { return false; }
    PollLocked(clock);
    if (alarm_alerts_ || tools_.TimerAlert()) { return false; }
    const bool has_alarm = std::any_of(tools_.Alarms().begin(), tools_.Alarms().end(),
        [](const WatchAlarm& alarm) { return alarm.enabled; });
    if (has_alarm && !WatchCalendarAt(clock.unix_ms, timezone_).valid) { return false; }
    if (tools_.TimerRunning()) { wake_delay_ms = tools_.TimerRemainingMs(clock.rtc_ms); }
    const uint64_t alarm_delay = WatchNextAlarmDelayMs(clock.unix_ms, timezone_, tools_.Alarms());
    if (alarm_delay && (!wake_delay_ms || alarm_delay < wake_delay_ms)) { wake_delay_ms = alarm_delay; }
    if ((has_alarm && !alarm_delay) || !SaveLocked(clock)) {
        wake_delay_ms = 0;
        return false;
    }
    sleep_clock_ = clock;
    sleep_wake_delay_ms_ = wake_delay_ms;
    sleep_prepared_ = true;
    return true;
}

bool WatchTimeService::SleepWakeDelay(const WatchClockReading& clock, uint64_t& wake_delay_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    wake_delay_ms = 0;
    if (!sleep_prepared_ || clock.rtc_epoch != sleep_clock_.rtc_epoch || clock.rtc_ms < sleep_clock_.rtc_ms) {
        return false;
    }
    if (!sleep_wake_delay_ms_) { return true; }
    const uint64_t elapsed = clock.rtc_ms - sleep_clock_.rtc_ms;
    if (sleep_clock_.unix_ms && elapsed <= std::numeric_limits<uint64_t>::max() - sleep_clock_.unix_ms) {
        const uint64_t expected_unix = sleep_clock_.unix_ms + elapsed;
        const uint64_t difference = clock.unix_ms >= expected_unix
            ? clock.unix_ms - expected_unix : expected_unix - clock.unix_ms;
        if (difference > 1000) {
            wake_delay_ms = 1;
            return true;
        }
    }
    // Expiry during shutdown must wake immediately, not become an unarmed timer.
    wake_delay_ms = elapsed >= sleep_wake_delay_ms_ ? 1 : sleep_wake_delay_ms_ - elapsed;
    return true;
}

void WatchTimeService::CancelSleep() {
    std::lock_guard<std::mutex> lock(mutex_);
    sleep_prepared_ = false;
}
