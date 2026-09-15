#include "watch_activity.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
uint32_t AddSaturated(uint32_t value, uint32_t increment) {
    return value + std::min(increment, std::numeric_limits<uint32_t>::max() - value);
}
}  // namespace

bool WatchActivity::Restore(const WatchActivityCheckpoint& checkpoint) {
    if (generation_ || !EncodeWatchActivityCheckpoint(checkpoint)) return false;
    saved_ = checkpoint;
    return true;
}

bool WatchActivity::BeginCounter(uint64_t generation, uint64_t now_ms, std::optional<int32_t> day) {
    if (counter_active_ || !generation || generation <= generation_) return false;
    generation_ = generation;
    last_ms_ = now_ms;
    counter_ = 0;
    day_ = day;
    status_ = WatchMotionStatus::kRunning;
    counter_active_ = true;
    return true;
}

void WatchActivity::Add(uint32_t steps, std::optional<int32_t> day) {
    if (!steps) return;
    if (!day) {
        saved_.undated_steps = AddSaturated(saved_.undated_steps, steps);
        return;
    }
    size_t index = 0;
    while (index < saved_.count && saved_.days[index].day != *day) ++index;
    if (index == saved_.count) {
        if (saved_.count == saved_.days.size()) {
            // Retain the most recently visited civil dates, including timezone changes.
            std::move(saved_.days.begin() + 1, saved_.days.end(), saved_.days.begin());
            --index;
        } else {
            ++saved_.count;
        }
        saved_.days[index] = {*day, 0};
    }
    saved_.days[index].steps = AddSaturated(saved_.days[index].steps, steps);
    const auto updated = saved_.days[index];
    std::move(saved_.days.begin() + index + 1, saved_.days.begin() + saved_.count,
        saved_.days.begin() + index);
    saved_.days[saved_.count - 1] = updated;
}

bool WatchActivity::Observe(uint64_t generation, uint32_t counter, uint64_t now_ms,
    std::optional<int32_t> day) {
    if (!counter_active_ || generation != generation_ ||
        counter >= kCounterModulus || now_ms <= last_ms_) return false;
    const uint64_t elapsed = now_ms - last_ms_;
    const uint32_t delta = (counter + kCounterModulus - counter_) % kCounterModulus;
    // Permit hardware batching of initial steps, but never turn a reset into 16M steps.
    const bool valid = elapsed <= kMaximumObservationGapMs && delta <= 32 + elapsed * 8 / 1000;
    if (valid) {
        // A counter delta spanning midnight or a clock correction has no timestamps.
        // Preserve it as undated instead of assigning it to the wrong day.
        Add(delta, day == day_ ? day : std::nullopt);
    } else {
        saved_.incomplete = true;
    }
    counter_ = counter;
    last_ms_ = now_ms;
    day_ = day;
    status_ = valid ? WatchMotionStatus::kRunning : WatchMotionStatus::kFault;
    return valid;
}

void WatchActivity::ReadFailed() {
    if (counter_active_) status_ = WatchMotionStatus::kFault;
}

void WatchActivity::Pause() {
    status_ = WatchMotionStatus::kPaused;
    counter_active_ = false;
    saved_.incomplete = true;
}

WatchActivitySnapshot WatchActivity::Snapshot(std::optional<int32_t> day) const {
    WatchActivitySnapshot result{status_, day, 0, saved_.undated_steps, saved_.incomplete};
    if (day) {
        for (size_t i = 0; i < saved_.count; ++i) {
            if (saved_.days[i].day == *day) result.steps = saved_.days[i].steps;
        }
    }
    return result;
}

void WatchRaiseToWake::Reset() {
    phase_ = Phase::kIdle;
    sampled_ = false;
    // Keep the last wake time: disabling the gesture must not bypass its cooldown.
}

bool WatchRaiseToWake::Observe(uint64_t now_ms, float screen_up_g, float magnitude_g, bool allowed) {
    const bool backwards = sampled_ && now_ms < last_ms_;
    const bool gap = sampled_ && now_ms - last_ms_ > 500;
    const bool duplicate = sampled_ && now_ms == last_ms_;
    if (backwards) woke_ = false;
    if (backwards || gap) phase_ = Phase::kIdle;
    last_ms_ = now_ms;
    sampled_ = true;
    const bool plausible = std::isfinite(screen_up_g) && std::isfinite(magnitude_g) &&
        magnitude_g >= 0.75f && magnitude_g <= 1.25f && std::abs(screen_up_g) <= magnitude_g;
    if (!allowed || !plausible || (woke_ && (now_ms < wake_ms_ || now_ms - wake_ms_ < 5000))) {
        phase_ = Phase::kIdle;
        return false;
    }
    if (duplicate) return false;
    const bool away = screen_up_g < 0.15f;
    const bool viewing = screen_up_g > 0.60f;
    if ((phase_ == Phase::kArmed || phase_ == Phase::kViewing) && now_ms - armed_ms_ > 3000) {
        phase_ = Phase::kIdle;
    }
    switch (phase_) {
        case Phase::kIdle:
            if (away) { phase_ = Phase::kAway; phase_ms_ = now_ms; }
            break;
        case Phase::kAway:
            if (!away) { phase_ = Phase::kIdle; }
            else if (now_ms - phase_ms_ >= 400) { phase_ = Phase::kArmed; armed_ms_ = now_ms; }
            break;
        case Phase::kArmed:
            if (viewing) { phase_ = Phase::kViewing; phase_ms_ = now_ms; }
            break;
        case Phase::kViewing:
            if (!viewing) { phase_ = Phase::kArmed; }
            else if (now_ms - phase_ms_ >= 350) {
                phase_ = Phase::kIdle;
                woke_ = true;
                wake_ms_ = now_ms;
                return true;
            }
            break;
    }
    return false;
}

namespace {
float Magnitude(const std::array<float, 3>& vector) {
    return std::sqrt(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2]);
}
bool Finite(const std::array<float, 3>& vector) {
    return std::isfinite(vector[0]) && std::isfinite(vector[1]) && std::isfinite(vector[2]);
}
}

void WatchScreenCalibration::Begin(uint64_t now_ms) {
    *this = WatchScreenCalibration{};
    started_ms_ = last_ms_ = now_ms;
    status_ = WatchCalibrationStatus::kCollecting;
}

void WatchScreenCalibration::Cancel() {
    *this = WatchScreenCalibration{};
}

std::optional<std::array<float, 3>> WatchScreenCalibration::Normal() const {
    return status_ == WatchCalibrationStatus::kReady ? std::optional(normal_) : std::nullopt;
}

void WatchScreenCalibration::Observe(const WatchImuReading* reading, uint64_t now_ms, bool allowed) {
    if (status_ != WatchCalibrationStatus::kCollecting) return;
    if (!allowed || now_ms < last_ms_ || now_ms - started_ms_ >= 15000) {
        status_ = WatchCalibrationStatus::kFailed;
        return;
    }
    const bool gap = now_ms - last_ms_ > 500;
    const bool duplicate = samples_ && now_ms == last_ms_;
    last_ms_ = now_ms;
    const std::array<float, 3> vector = reading
        ? std::array<float, 3>{reading->x_g, reading->y_g, reading->z_g} : std::array<float, 3>{};
    const float magnitude = Magnitude(vector);
    if (!reading || !reading->generation || !Finite(vector) || magnitude < 0.85f || magnitude > 1.15f) {
        samples_ = 0;
        return;
    }
    if (gap || reading->generation != generation_) samples_ = 0;
    generation_ = reading->generation;
    if (duplicate && samples_) return;
    bool stable = true;
    for (size_t i = 0; i < 3; ++i) stable &= std::abs(vector[i] - first_[i]) <= 0.05f;
    if (!stable) samples_ = 0;
    if (!samples_) {
        window_ms_ = now_ms;
        first_ = vector;
        sum_ = {};
    }
    if (samples_ == 256) { status_ = WatchCalibrationStatus::kFailed; return; }
    ++samples_;
    for (size_t i = 0; i < 3; ++i) sum_[i] += vector[i];
    if (samples_ < 9 || now_ms - window_ms_ < 2000) return;
    const float length = Magnitude(sum_);
    if (!std::isfinite(length) || length <= 0) { status_ = WatchCalibrationStatus::kFailed; return; }
    for (size_t i = 0; i < 3; ++i) normal_[i] = sum_[i] / length;
    status_ = WatchCalibrationStatus::kReady;
}

bool WatchCalibratedRaiseToWake::SetScreenNormal(const std::array<float, 3>& normal) {
    const float length = Magnitude(normal);
    if (!Finite(normal) || length < 0.98f || length > 1.02f) return false;
    for (size_t i = 0; i < 3; ++i) normal_[i] = normal[i] / length;
    calibrated_ = true;
    generation_ = 0;
    gesture_.Reset();
    return true;
}

bool WatchCalibratedRaiseToWake::SetEnabled(bool enabled) {
    if (enabled && !calibrated_) return false;
    enabled_ = enabled;
    gesture_.Reset();
    return true;
}

bool WatchCalibratedRaiseToWake::Observe(const WatchImuReading* reading, uint64_t now_ms, bool allowed) {
    if (!allowed || !enabled_ || !reading || !reading->generation) {
        gesture_.Reset();
        return false;
    }
    if (generation_ != reading->generation) {
        generation_ = reading->generation;
        gesture_.Reset();
    }
    const std::array<float, 3> vector{reading->x_g, reading->y_g, reading->z_g};
    if (!Finite(vector)) { gesture_.Reset(); return false; }
    float screen_up = 0;
    for (size_t i = 0; i < 3; ++i) screen_up += vector[i] * normal_[i];
    const float magnitude = Magnitude(vector);
    // A rounded unit normal can overshoot the Cauchy-Schwarz bound by a few ulps.
    screen_up = std::clamp(screen_up, -magnitude, magnitude);
    return gesture_.Observe(now_ms, screen_up, magnitude, true);
}
