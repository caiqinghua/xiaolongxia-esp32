#include "watch_ble_advertising_policy.h"

#include <limits>

void WatchBleAdvertisingPolicy::Reset() {
    state_ = State::kStopped;
    last_ms_ = 0;
    deadline_ms_ = 0;
    start_attempts_ = 0;
    stop_attempts_ = 0;
}

bool WatchBleAdvertisingPolicy::CheckClock(int64_t now_ms) {
    if (now_ms < 0 || now_ms < last_ms_) {
        Reset();
        return false;
    }
    last_ms_ = now_ms;
    return true;
}

void WatchBleAdvertisingPolicy::Begin(int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    Reset();
    if (now_ms < 0) return;
    last_ms_ = now_ms;
    state_ = State::kAwaitingData;
}

void WatchBleAdvertisingPolicy::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    Reset();
}

WatchBleAdvertisingAction WatchBleAdvertisingPolicy::ScheduleStart(bool fast, int64_t now_ms) {
    if (start_attempts_ >= kMaxStartAttempts) {
        Reset();
        return WatchBleAdvertisingAction::kNone;
    }
    ++start_attempts_;
    state_ = fast ? State::kStartingFast : State::kStartingIdle;
    last_ms_ = now_ms;
    return fast ? WatchBleAdvertisingAction::kStartFast : WatchBleAdvertisingAction::kStartIdle;
}

WatchBleAdvertisingAction WatchBleAdvertisingPolicy::DataConfigured(bool success, int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!CheckClock(now_ms) || state_ != State::kAwaitingData) return WatchBleAdvertisingAction::kNone;
    if (!success) {
        Reset();
        return WatchBleAdvertisingAction::kNone;
    }
    start_attempts_ = 0;
    return ScheduleStart(true, now_ms);
}

WatchBleAdvertisingAction WatchBleAdvertisingPolicy::StartCompleted(bool success, int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!CheckClock(now_ms)) return WatchBleAdvertisingAction::kNone;
    const bool fast = state_ == State::kStartingFast;
    if (!fast && state_ != State::kStartingIdle) return WatchBleAdvertisingAction::kNone;
    if (!success) {
        if (start_attempts_ >= kMaxStartAttempts ||
            now_ms > std::numeric_limits<int64_t>::max() - kRetryMs) {
            Reset();
            return WatchBleAdvertisingAction::kNone;
        }
        state_ = fast ? State::kRetryFast : State::kRetryIdle;
        deadline_ms_ = now_ms + kRetryMs;
        return WatchBleAdvertisingAction::kNone;
    }
    start_attempts_ = 0;
    if (fast) {
        if (now_ms > std::numeric_limits<int64_t>::max() - kFastDurationMs) {
            Reset();
            return WatchBleAdvertisingAction::kNone;
        }
        state_ = State::kFast;
        deadline_ms_ = now_ms + kFastDurationMs;
        stop_attempts_ = 0;
    } else {
        state_ = State::kIdle;
        deadline_ms_ = 0;
    }
    return WatchBleAdvertisingAction::kNone;
}

WatchBleAdvertisingAction WatchBleAdvertisingPolicy::StopCompleted(bool success, int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!CheckClock(now_ms) || state_ != State::kStoppingFast) return WatchBleAdvertisingAction::kNone;
    if (!success) {
        state_ = State::kFast;
        deadline_ms_ = stop_attempts_ >= kMaxStopAttempts
            ? std::numeric_limits<int64_t>::max()
            : now_ms > std::numeric_limits<int64_t>::max() - kRetryMs
            ? std::numeric_limits<int64_t>::max()
            : now_ms + kRetryMs;
        return WatchBleAdvertisingAction::kNone;
    }
    start_attempts_ = 0;
    return ScheduleStart(false, now_ms);
}

WatchBleAdvertisingAction WatchBleAdvertisingPolicy::OperationRejected(
    WatchBleAdvertisingAction action,
    int64_t now_ms
) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!CheckClock(now_ms)) return WatchBleAdvertisingAction::kNone;
    const bool rejected_fast = action == WatchBleAdvertisingAction::kStartFast && state_ == State::kStartingFast;
    const bool rejected_idle = action == WatchBleAdvertisingAction::kStartIdle && state_ == State::kStartingIdle;
    if (rejected_fast || rejected_idle) {
        if (start_attempts_ >= kMaxStartAttempts ||
            now_ms > std::numeric_limits<int64_t>::max() - kRetryMs) {
            Reset();
            return WatchBleAdvertisingAction::kNone;
        }
        state_ = rejected_fast ? State::kRetryFast : State::kRetryIdle;
        deadline_ms_ = now_ms + kRetryMs;
    } else if (action == WatchBleAdvertisingAction::kStop && state_ == State::kStoppingFast) {
        state_ = State::kFast;
        deadline_ms_ = stop_attempts_ >= kMaxStopAttempts
            ? std::numeric_limits<int64_t>::max()
            : now_ms > std::numeric_limits<int64_t>::max() - kRetryMs
            ? std::numeric_limits<int64_t>::max()
            : now_ms + kRetryMs;
    }
    return WatchBleAdvertisingAction::kNone;
}

WatchBleAdvertisingAction WatchBleAdvertisingPolicy::Poll(int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!CheckClock(now_ms)) return WatchBleAdvertisingAction::kNone;
    if (state_ == State::kFast && now_ms >= deadline_ms_) {
        if (stop_attempts_ >= kMaxStopAttempts) return WatchBleAdvertisingAction::kNone;
        ++stop_attempts_;
        state_ = State::kStoppingFast;
        return WatchBleAdvertisingAction::kStop;
    }
    if (now_ms < deadline_ms_) return WatchBleAdvertisingAction::kNone;
    if (state_ == State::kRetryFast) return ScheduleStart(true, now_ms);
    if (state_ == State::kRetryIdle) return ScheduleStart(false, now_ms);
    return WatchBleAdvertisingAction::kNone;
}
