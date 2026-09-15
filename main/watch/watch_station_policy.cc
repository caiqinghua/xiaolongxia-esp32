#include "watch_station_policy.h"
#include <algorithm>

void WatchStationPolicy::Request(uint64_t now_ms) {
    initialized_ = true;
    hold_until_ms_ = now_ms + 90000;
    reconnect_after_ms_ = 0;
    attempts_ = 0;
    failed_ = false;
}

void WatchStationPolicy::CancelDemand() {
    initialized_ = true;
    hold_until_ms_ = 0;
}

void WatchStationPolicy::KeepForConversation(uint64_t now_ms) {
    initialized_ = true;
    hold_until_ms_ = std::max(hold_until_ms_, now_ms + 30000);
}

WatchStationAction WatchStationPolicy::Poll(uint64_t now_ms, bool enabled, bool continuous,
                                            bool busy, bool connected, bool provisioning) {
    if (!initialized_) Request(now_ms);
    if (!enabled) {
        CancelDemand();
        waiting_ = false;
        return WatchStationAction::kNone;
    }
    if (connected) {
        suspended_ = false;
        waiting_ = false;
        attempts_ = 0;
    } else if (waiting_ && now_ms >= association_deadline_ms_) {
        waiting_ = false;
        cleanup_ = failed_ = true;
        hold_until_ms_ = 0;
        reconnect_after_ms_ = now_ms + 60000;
    }
    if (busy || provisioning || now_ms < next_operation_ms_) return WatchStationAction::kNone;
    const bool wanted = continuous || now_ms < hold_until_ms_;
    if (cleanup_ || (!wanted && !suspended_)) return WatchStationAction::kStop;
    if (wanted && !connected && !waiting_ && now_ms >= reconnect_after_ms_)
        return WatchStationAction::kStart;
    return WatchStationAction::kNone;
}

void WatchStationPolicy::Complete(WatchStationAction action, bool succeeded, uint64_t now_ms) {
    if (action == WatchStationAction::kNone) return;
    next_operation_ms_ = succeeded ? 0 : now_ms + 2000;
    if (action == WatchStationAction::kStop) {
        if (succeeded) {
            suspended_ = true;
            waiting_ = cleanup_ = false;
        } else failed_ = true;
        return;
    }
    if (succeeded) {
        suspended_ = false;
        waiting_ = true;
        association_deadline_ms_ = now_ms + 60000;
    } else if (++attempts_ >= 3) {
        failed_ = cleanup_ = true;
        hold_until_ms_ = 0;
        next_operation_ms_ = 0;
        reconnect_after_ms_ = now_ms + 60000;
    }
}
