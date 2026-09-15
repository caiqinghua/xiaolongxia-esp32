#include "watch_station_recovery.h"

void WatchStationRecovery::Request() {
    pending_ = true;
    attempts_ = 0;
    last_attempt_ms_ = 0;
}

void WatchStationRecovery::Cancel() {
    pending_ = false;
    attempts_ = 0;
}

WatchStationRecoveryResult WatchStationRecovery::Poll(uint64_t now_ms, bool enabled, bool ready,
    AudioSessionCoordinator& coordinator, const std::function<bool()>& restore) {
    if (!pending_) return WatchStationRecoveryResult::kIdle;
    if (!enabled) {
        Cancel();
        return WatchStationRecoveryResult::kCanceled;
    }
    if (!ready || !restore || (attempts_ &&
        (now_ms < last_attempt_ms_ || now_ms - last_attempt_ms_ < 2000)))
        return WatchStationRecoveryResult::kDeferred;
    const auto reservation = coordinator.Begin(AudioSessionMode::kMaintenance);
    if (!reservation) return WatchStationRecoveryResult::kDeferred;
    const bool started = restore();
    coordinator.Fail(reservation, true);
    if (started) {
        Cancel();
        return WatchStationRecoveryResult::kStarted;
    }
    last_attempt_ms_ = now_ms;
    if (++attempts_ < 3) return WatchStationRecoveryResult::kDeferred;
    Cancel();
    return WatchStationRecoveryResult::kFailed;
}
