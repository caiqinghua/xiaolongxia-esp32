#include "watch_wrist_control.h"

#include <cmath>
#include <utility>

std::optional<WatchWristRecord> EncodeWatchWristPreferences(const WatchWristPreferences& preferences) {
    WatchCalibratedRaiseToWake validator;
    if ((preferences.normal && !validator.SetScreenNormal(*preferences.normal)) ||
        !validator.SetEnabled(preferences.enabled)) return std::nullopt;
    WatchWristRecord record{};
    record[0] = 1;
    record[1] = preferences.enabled;
    record[2] = preferences.normal.has_value();
    if (preferences.normal) {
        for (size_t axis = 0; axis < 3; ++axis) {
            const auto value = static_cast<uint32_t>(static_cast<int32_t>(std::lround((*preferences.normal)[axis] * 10000)));
            for (size_t byte = 0; byte < 4; ++byte) record[4 + axis * 4 + byte] = static_cast<uint8_t>(value >> (byte * 8));
        }
    }
    return record;
}

bool DecodeWatchWristPreferences(const WatchWristRecord& record, WatchWristPreferences& preferences) {
    if (record[0] != 1 || record[1] > 1 || record[2] > 1 || record[3]) return false;
    WatchWristPreferences next;
    next.enabled = record[1] != 0;
    std::array<float, 3> normal{};
    for (size_t axis = 0; axis < 3; ++axis) {
        uint32_t value = 0;
        for (size_t byte = 0; byte < 4; ++byte) value |= uint32_t(record[4 + axis * 4 + byte]) << (byte * 8);
        const int64_t signed_value = value <= INT32_MAX ? int64_t(value) : int64_t(value) - (1LL << 32);
        if (signed_value < -10200 || signed_value > 10200 || (!record[2] && value)) return false;
        normal[axis] = static_cast<float>(signed_value) / 10000;
    }
    if (record[2]) next.normal = normal;
    if (!EncodeWatchWristPreferences(next)) return false;
    preferences = next;
    return true;
}

WatchWristControl::WatchWristControl(Hooks hooks) : hooks_(std::move(hooks)) {
    state_.available = bool(hooks_.load) && bool(hooks_.save) && bool(hooks_.wake);
}

WatchWristSnapshot WatchWristControl::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool WatchWristControl::Request(uint64_t id, WatchWristAction action, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool cancel = action == WatchWristAction::kCancel || (action == WatchWristAction::kSetEnabled && !enabled);
    if (!state_.available || loading_ || !id || id <= state_.request || (state_.pending && !cancel) ||
        (action != WatchWristAction::kCalibrate && action != WatchWristAction::kSetEnabled && action != WatchWristAction::kCancel) ||
        (!state_.loaded && !state_.storage_error) ||
        (action == WatchWristAction::kSetEnabled && enabled && (!state_.loaded || !saved_.normal))) return false;
    action_ = action;
    desired_enabled_ = enabled;
    state_.request = id;
    state_.pending = true;
    state_.succeeded = false;
    if (cancel || action == WatchWristAction::kCalibrate) {
        gesture_.SetEnabled(false);
        state_.enabled = false;
        calibration_.Cancel();
        state_.calibration = WatchCalibrationStatus::kIdle;
    }
    return true;
}

void WatchWristControl::ApplyLocked(const WatchWristPreferences& preferences) {
    if (preferences.normal) gesture_.SetScreenNormal(*preferences.normal);
    gesture_.SetEnabled(preferences.enabled);
    state_.enabled = preferences.enabled;
    state_.calibrated = preferences.normal.has_value();
}

void WatchWristControl::Poll(const WatchImuReading* reading, uint64_t now_ms, bool allowed, bool display_idle) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!state_.available) return;
    if (!state_.loaded && !state_.pending && (!load_attempted_ || now_ms < load_ms_ || now_ms - load_ms_ >= 30000)) {
        loading_ = load_attempted_ = true;
        load_ms_ = now_ms;
        lock.unlock();
        WatchWristPreferences preferences;
        const auto result = hooks_.load(preferences);
        lock.lock();
        loading_ = false;
        if (result == WatchWristLoad::kAbsent) preferences = {};
        state_.loaded = (result == WatchWristLoad::kAbsent || result == WatchWristLoad::kLoaded) &&
            bool(EncodeWatchWristPreferences(preferences));
        state_.storage_error = !state_.loaded;
        if (state_.loaded) { saved_ = preferences; ApplyLocked(saved_); }
    }
    std::optional<WatchWristPreferences> write;
    const auto request = state_.request;
    if (state_.pending) {
        if (action_ == WatchWristAction::kCalibrate) {
            if (calibration_request_ != request) {
                calibration_request_ = request;
                calibration_.Begin(now_ms);
            }
            calibration_.Observe(reading, now_ms, allowed);
            state_.calibration = calibration_.Status();
            if (calibration_.Normal()) write = WatchWristPreferences{false, calibration_.Normal()};
            else if (state_.calibration == WatchCalibrationStatus::kFailed) state_.pending = false;
        } else {
            const bool enabled = action_ == WatchWristAction::kSetEnabled && desired_enabled_;
            if (enabled && !allowed) state_.pending = false;
            else write = WatchWristPreferences{enabled, saved_.normal};
        }
    }
    if (write) {
        lock.unlock();
        const bool stored = hooks_.save(*write);
        lock.lock();
        if (state_.request == request) {
            state_.pending = false;
            state_.succeeded = stored;
            state_.storage_error = !stored;
            if (stored) { saved_ = *write; state_.loaded = true; ApplyLocked(saved_); }
            else { gesture_.SetEnabled(false); state_.enabled = false; }
        }
    }
    // Only the worker calls the bounded power callback, with cancellation serialized.
    if (gesture_.Observe(reading, now_ms, allowed && display_idle && !state_.pending)) hooks_.wake();
}
