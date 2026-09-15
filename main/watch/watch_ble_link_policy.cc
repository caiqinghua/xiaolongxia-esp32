#include "watch_ble_link_policy.h"

#include <algorithm>
#include <limits>

void WatchBleLinkPolicy::Reset() {
    connected_ = false;
    address_ = {};
    desired_active_.reset();
    completed_active_.reset();
    pending_active_.reset();
    attempts_ = 0;
    last_ms_ = active_until_ms_ = next_request_ms_ = response_deadline_ms_ = 0;
}

bool WatchBleLinkPolicy::CheckClock(int64_t now_ms) {
    if (!connected_) return false;
    if (now_ms < last_ms_) return false;
    if (now_ms > std::numeric_limits<int64_t>::max() - kActivityMs) {
        Reset();
        return false;
    }
    last_ms_ = now_ms;
    return true;
}

void WatchBleLinkPolicy::Connected(const uint8_t* address, int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    Reset();
    if (!address || now_ms < 0 || now_ms > std::numeric_limits<int64_t>::max() - kActivityMs) return;
    std::copy_n(address, address_.size(), address_.begin());
    connected_ = true;
    last_ms_ = now_ms;
    active_until_ms_ = now_ms + kActivityMs;
    next_request_ms_ = now_ms + kInitialDelayMs;
}

void WatchBleLinkPolicy::Disconnected() {
    std::lock_guard<std::mutex> lock(mutex_);
    Reset();
}

void WatchBleLinkPolicy::Activity(int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (CheckClock(now_ms)) active_until_ms_ = now_ms + kActivityMs;
}

void WatchBleLinkPolicy::Poll(int64_t now_ms, bool busy, const Submit& submit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!CheckClock(now_ms) || !submit) return;
    if (busy) active_until_ms_ = now_ms + kActivityMs;
    const bool active = now_ms < active_until_ms_;
    if (!desired_active_ || *desired_active_ != active) {
        desired_active_ = active;
        attempts_ = 0;
    }
    if (pending_active_) {
        if (now_ms < response_deadline_ms_) return;
        pending_active_.reset();
        next_request_ms_ = now_ms + kRetryMs;
    }
    if ((completed_active_ && *completed_active_ == active) || attempts_ >= kMaxAttempts ||
        now_ms < next_request_ms_) return;
    ++attempts_;
    next_request_ms_ = now_ms + kRetryMs;
    // The SDK submit operation must enqueue without invoking Updated synchronously.
    if (submit(address_, active ? kActive : kIdle)) {
        completed_active_.reset();
        pending_active_ = active;
        response_deadline_ms_ = now_ms + kResponseMs;
    }
}

bool WatchBleLinkPolicy::Updated(const uint8_t* address, uint16_t minimum_interval, uint16_t maximum_interval,
                                 bool success, int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!address || !connected_ || !std::equal(address_.begin(), address_.end(), address) ||
        !CheckClock(now_ms) || !pending_active_) return false;
    const auto expected = *pending_active_ ? kActive : kIdle;
    if (minimum_interval != expected.minimum_interval || maximum_interval != expected.maximum_interval) return false;
    if (success) completed_active_ = pending_active_;
    pending_active_.reset();
    next_request_ms_ = now_ms + kRetryMs;
    return true;
}
