#include "watch/watch_activity.h"

#include <cassert>
#include <cstdio>
#include <limits>
#include <cmath>

namespace {
void TestCounterLifecycle() {
    WatchActivity activity;
    assert(activity.Snapshot(20000).status == WatchMotionStatus::kUnavailable);
    assert(!activity.BeginCounter(0, 0, 20000));
    assert(activity.BeginCounter(1, 0, 20000));
    assert(activity.Observe(1, 8, 1000, 20000));
    assert(activity.Snapshot(20000).steps == 8);
    assert(!activity.BeginCounter(2, 1000, 20000));
    activity.ReadFailed();
    assert(activity.Snapshot(20000).status == WatchMotionStatus::kFault);
    assert(activity.Observe(1, 12, 2000, 20000));
    assert(activity.Snapshot(20000).steps == 12);
    assert(!activity.Observe(1, 20, 2000, 20000));
    assert(!activity.Observe(2, 20, 3000, 20000));
    assert(!activity.Observe(1, WatchActivity::kCounterModulus, 3000, 20000));
    activity.Pause();
    activity.ReadFailed();
    assert(activity.Snapshot(20000).status == WatchMotionStatus::kPaused);
    assert(!activity.Observe(1, 20, 3000, 20000));
    assert(!activity.BeginCounter(1, 3000, 20000));
    assert(activity.BeginCounter(2, 3000, 20000));
    assert(activity.Observe(2, 3, 4000, 20000));
    assert(activity.Snapshot(20000).steps == 15);
    assert(activity.Snapshot(20000).incomplete);
}

void TestCounterResetWrapAndGap() {
    WatchActivity activity;
    assert(activity.BeginCounter(1, 0, 1));
    assert(activity.Observe(1, 30, 1000, 1));
    assert(!activity.Observe(1, 0, 2000, 1));
    assert(activity.Snapshot(1).steps == 30);
    assert(activity.Observe(1, 4, 3000, 1));
    assert(!activity.Observe(1, 100, 40000, 1));
    assert(activity.Observe(1, 103, 41000, 1));
    assert(activity.Snapshot(1).steps == 37);
    // A rejected jump rebases the hardware counter without adding phantom steps.
    assert(!activity.Observe(1, WatchActivity::kCounterModulus - 2, 42000, 1));
    assert(activity.Observe(1, 1, 43000, 1));
    assert(activity.Snapshot(1).steps == 40);
    assert(!activity.Observe(1, 2, 42999, 1));
    assert(activity.Observe(1, 2, 44000, 1));
    assert(activity.Snapshot(1).steps == 41);
}

void TestDateBoundariesAndHistory() {
    WatchActivity activity;
    assert(activity.BeginCounter(1, 0, std::nullopt));
    assert(activity.Observe(1, 3, 1000, std::nullopt));
    assert(activity.Observe(1, 5, 2000, 1));
    assert(activity.Observe(1, 10, 3000, 1));
    assert(activity.Observe(1, 12, 4000, 2));
    assert(activity.Observe(1, 15, 5000, 2));
    assert(activity.Observe(1, 17, 6000, 1));
    assert(activity.Observe(1, 20, 7000, 1));
    assert(activity.Snapshot(1).steps == 8);
    assert(activity.Snapshot(2).steps == 3);
    assert(activity.Snapshot(2).undated_steps == 9);
    auto checkpoint = activity.Checkpoint();
    assert(checkpoint.count == 2);
    WatchActivity restored;
    assert(restored.Restore(checkpoint));
    assert(restored.Snapshot(1).steps == 8);
    assert(restored.BeginCounter(1, 0, 2));
    assert(!restored.Restore(checkpoint));
    uint32_t counter = 0;
    uint64_t now = 0;
    for (int32_t day = 2; day <= 10; ++day) {
        assert(restored.Observe(1, ++counter, now += 1000, day));
        assert(restored.Observe(1, ++counter, now += 1000, day));
    }
    checkpoint = restored.Checkpoint();
    assert(checkpoint.count == 7 && checkpoint.days[0].day == 4 && checkpoint.days[6].day == 10);
    assert(restored.Snapshot(1).steps == 0);
    checkpoint.days[1].day = checkpoint.days[0].day;
    WatchActivity invalid;
    assert(!invalid.Restore(checkpoint));
    checkpoint.count = 8;
    assert(!invalid.Restore(checkpoint));
}

void TestSaturationAndPersistence() {
    WatchActivity activity;
    WatchActivityCheckpoint checkpoint;
    checkpoint.count = 1;
    checkpoint.days[0] = {42, UINT32_MAX - 1};
    checkpoint.undated_steps = UINT32_MAX - 1;
    checkpoint.incomplete = true;
    assert(activity.Restore(checkpoint));
    assert(activity.BeginCounter(1, 0, 42));
    assert(activity.Observe(1, 3, 1000, 42));
    assert(activity.Observe(1, 6, 2000, std::nullopt));
    assert(activity.Snapshot(42).steps == UINT32_MAX);
    assert(activity.Snapshot(42).undated_steps == UINT32_MAX);
    assert(activity.Snapshot(42).incomplete);
}

void TestCheckpointRecord() {
    WatchActivityCheckpoint original;
    original.count = 3;
    original.days[0] = {INT32_MIN, UINT32_MAX};
    original.days[1] = {-1, 777};
    original.days[2] = {INT32_MAX, 888};
    original.undated_steps = 123;
    original.incomplete = true;
    const auto encoded = EncodeWatchActivityCheckpoint(original);
    assert(encoded && encoded->size() == 64);
    WatchActivityCheckpoint decoded;
    assert(DecodeWatchActivityCheckpoint(encoded->data(), encoded->size(), decoded));
    assert(decoded.count == 3 && decoded.days[0].day == INT32_MIN && decoded.days[0].steps == UINT32_MAX);
    assert(decoded.days[1].day == -1 && decoded.days[1].steps == 777);
    assert(decoded.days[2].day == INT32_MAX && decoded.days[2].steps == 888);
    assert(decoded.undated_steps == 123 && decoded.incomplete);
    for (size_t size = 0; size < encoded->size(); ++size) {
        assert(!DecodeWatchActivityCheckpoint(encoded->data(), size, decoded));
    }
    assert(!DecodeWatchActivityCheckpoint(encoded->data(), encoded->size() + 1, decoded));
    assert(!DecodeWatchActivityCheckpoint(nullptr, encoded->size(), decoded));
    for (size_t offset : {0, 1, 2, 3, 63}) {
        auto corrupted = *encoded;
        corrupted[offset] = 0xff;
        assert(!DecodeWatchActivityCheckpoint(corrupted.data(), corrupted.size(), decoded));
        assert(decoded.count == 3 && decoded.undated_steps == 123);
    }
    auto duplicate = *encoded;
    for (size_t i = 0; i < 4; ++i) duplicate[16 + i] = duplicate[8 + i];
    assert(!DecodeWatchActivityCheckpoint(duplicate.data(), duplicate.size(), decoded));
    original.days[1].day = original.days[0].day;
    assert(!EncodeWatchActivityCheckpoint(original));
    original.count = 8;
    assert(!EncodeWatchActivityCheckpoint(original));
    assert(EncodeWatchActivityCheckpoint({}));
}

bool Gesture(WatchRaiseToWake& gesture, uint64_t start, bool allowed = true) {
    assert(!gesture.Observe(start, 0, 1, allowed));
    assert(!gesture.Observe(start + 200, 0, 1, allowed));
    assert(!gesture.Observe(start + 400, 0, 1, allowed));
    assert(!gesture.Observe(start + 600, 0.8f, 1, allowed));
    assert(!gesture.Observe(start + 800, 0.8f, 1, allowed));
    return gesture.Observe(start + 1000, 0.8f, 1, allowed);
}

void TestRaiseToWake() {
    WatchRaiseToWake gesture;
    assert(Gesture(gesture, 0));
    for (uint64_t now = 1200; now < 6000; now += 200) assert(!gesture.Observe(now, 0.8f, 1, true));
    assert(!Gesture(gesture, 6000, false));
    assert(Gesture(gesture, 8000));
    gesture.Reset();
    assert(!Gesture(gesture, 10000));
    // Holding a viewing pose cannot repeatedly wake the display after cooldown.
    for (uint64_t now = 12000; now < 20000; now += 200) assert(!gesture.Observe(now, 0.8f, 1, true));
    assert(Gesture(gesture, 20000));
    assert(!gesture.Observe(22000, std::numeric_limits<float>::quiet_NaN(), 1, true));
    assert(!gesture.Observe(23000, 0.8f, 0.1f, true));
    assert(!gesture.Observe(24000, 0.8f, 4, true));
}

void TestGestureGapsAndInterruptedDwell() {
    WatchRaiseToWake gesture;
    assert(!gesture.Observe(0, 0, 1, true));
    assert(!gesture.Observe(200, 0, 1, true));
    assert(!gesture.Observe(400, 0, 1, true));
    assert(!gesture.Observe(600, 0.8f, 1, true));
    assert(!gesture.Observe(1000, 0.8f, 1, false));
    assert(!gesture.Observe(1200, 0.8f, 1, true));
    assert(Gesture(gesture, 2000));
    gesture.Reset();
    assert(!gesture.Observe(10000, 0, 1, true));
    assert(!gesture.Observe(10200, 0, 1, true));
    assert(!gesture.Observe(10400, 0, 1, true));
    assert(!gesture.Observe(10600, 0.8f, 1, true));
    assert(!gesture.Observe(11600, 0.8f, 1, true));
    assert(!gesture.Observe(12000, 0.8f, 1, true));
    assert(Gesture(gesture, 100));  // Rebase after a monotonic-clock reset.
}

void TestScreenNormalCalibration() {
    for (const std::array<float, 3> normal : {
        std::array<float, 3>{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0.6f, 0, 0.8f}}) {
        WatchScreenCalibration calibration;
        assert(!calibration.Normal());
        calibration.Begin(1000);
        WatchImuReading sample{1, 0, normal[0], normal[1], normal[2]};
        for (uint64_t now = 1000; now <= 3000; now += 250) {
            calibration.Observe(&sample, now, true);
            if (now < 3000) assert(!calibration.Normal());
        }
        assert(calibration.Status() == WatchCalibrationStatus::kReady);
        const auto result = *calibration.Normal();
        for (size_t i = 0; i < 3; ++i) assert(std::abs(result[i] - normal[i]) < 0.0001f);
        WatchCalibratedRaiseToWake gesture;
        assert(gesture.SetScreenNormal(result) && gesture.SetEnabled(true));
        std::array<float, 3> perpendicular{-normal[1], normal[0], 0};
        float length = std::sqrt(perpendicular[0] * perpendicular[0] + perpendicular[1] * perpendicular[1]);
        if (length == 0) { perpendicular = {1, 0, 0}; length = 1; }
        for (auto& value : perpendicular) value /= length;
        WatchImuReading away{1, 0, perpendicular[0], perpendicular[1], perpendicular[2]};
        WatchImuReading view{1, 0, 0.8f * normal[0] + 0.6f * perpendicular[0],
            0.8f * normal[1] + 0.6f * perpendicular[1], 0.8f * normal[2] + 0.6f * perpendicular[2]};
        for (uint64_t now = 0; now < 1000; now += 200) assert(!gesture.Observe(now < 600 ? &away : &view, now, true));
        assert(gesture.Observe(&view, 1000, true));
        // A completed result is immutable until an explicit new calibration.
        calibration.Observe(nullptr, 20000, false);
        assert(calibration.Normal() == result);
        calibration.Cancel();
        assert(calibration.Status() == WatchCalibrationStatus::kIdle && !calibration.Normal());
    }
}

void TestCalibrationRejectsMotionAndDiscontinuity() {
    WatchImuReading sample{1, 0, 0, 0, 1};
    for (int interruption = 0; interruption < 5; ++interruption) {
        WatchScreenCalibration calibration;
        calibration.Begin(0);
        for (uint64_t now = 0; now <= 1500; now += 250) calibration.Observe(&sample, now, true);
        if (interruption == 0) calibration.Observe(nullptr, 1750, true);
        if (interruption == 1) {
            auto invalid = sample; invalid.x_g = std::numeric_limits<float>::quiet_NaN();
            calibration.Observe(&invalid, 1750, true);
        }
        if (interruption == 2) {
            auto moved = sample; moved.z_g = 0.8f; moved.x_g = 0.6f;
            calibration.Observe(&moved, 1750, true);
        }
        if (interruption == 3) sample.generation++;
        // The gap case deliberately misses more than 500 ms of samples.
        const uint64_t restart = interruption == 4 ? 2250 : 2000;
        for (uint64_t now = restart; now < restart + 2000; now += 250) {
            calibration.Observe(&sample, now, true);
            assert(!calibration.Normal());
        }
        calibration.Observe(&sample, restart + 2000, true);
        assert(calibration.Normal());
    }
    WatchScreenCalibration calibration;
    calibration.Begin(0);
    for (int i = 0; i < 30; ++i) calibration.Observe(&sample, 0, true);
    assert(!calibration.Normal());
    calibration.Observe(&sample, 15000, true);
    assert(calibration.Status() == WatchCalibrationStatus::kFailed && !calibration.Normal());
    calibration.Begin(1000);
    calibration.Observe(&sample, 999, true);
    assert(calibration.Status() == WatchCalibrationStatus::kFailed);
    calibration.Begin(0);
    calibration.Observe(&sample, 250, false);
    assert(calibration.Status() == WatchCalibrationStatus::kFailed);
    for (const float magnitude : {0.0f, 0.8f, 1.2f, std::numeric_limits<float>::infinity()}) {
        calibration.Begin(0); sample.z_g = magnitude;
        for (uint64_t now = 0; now <= 2000; now += 250) calibration.Observe(&sample, now, true);
        assert(!calibration.Normal());
    }
}

bool CalibratedGesture(WatchCalibratedRaiseToWake& gesture, uint64_t start, uint64_t generation = 1, bool allowed = true) {
    WatchImuReading away{generation, 0, 0, 0, 1};
    WatchImuReading view{generation, 0, -0.8f, 0, 0.6f};
    for (uint64_t offset = 0; offset < 1000; offset += 200) {
        assert(!gesture.Observe(offset < 600 ? &away : &view, start + offset, allowed));
    }
    return gesture.Observe(&view, start + 1000, allowed);
}

void TestCalibratedRaisePolicy() {
    WatchCalibratedRaiseToWake gesture;
    assert(!gesture.Enabled() && !gesture.Calibrated() && !gesture.SetEnabled(true));
    assert(!CalibratedGesture(gesture, 0));
    assert(!gesture.SetScreenNormal({0, 0, 0}));
    assert(!gesture.SetScreenNormal({0, 0, 2}));
    assert(!gesture.SetScreenNormal({0, 0, std::numeric_limits<float>::quiet_NaN()}));
    assert(gesture.SetScreenNormal({-1, 0, 0}));
    assert(!gesture.Enabled() && !CalibratedGesture(gesture, 2000));
    assert(gesture.SetEnabled(true));
    assert(CalibratedGesture(gesture, 4000));
    assert(gesture.SetEnabled(false) && gesture.SetEnabled(true));
    assert(!CalibratedGesture(gesture, 6000)); // Consent toggles do not bypass cooldown.
    assert(!CalibratedGesture(gesture, 11000, 1, false));
    assert(CalibratedGesture(gesture, 13000));
    WatchImuReading away{1, 0, 0, 0, 1}, view{1, 0, -0.8f, 0, 0.6f};
    for (const bool sensor_restart : {false, true}) {
        const uint64_t start = sensor_restart ? 28000 : 21000;
        for (uint64_t offset = 0; offset <= 400; offset += 200) assert(!gesture.Observe(&away, start + offset, true));
        assert(!gesture.Observe(&view, start + 600, true));
        if (sensor_restart) ++view.generation;
        else assert(!gesture.Observe(nullptr, start + 700, true));
        assert(!gesture.Observe(&view, start + 800, true));
        assert(!gesture.Observe(&view, start + 1000, true));
    }
}
}  // namespace

int main() {
    TestCounterLifecycle();
    TestCounterResetWrapAndGap();
    TestDateBoundariesAndHistory();
    TestSaturationAndPersistence();
    TestCheckpointRecord();
    TestRaiseToWake();
    TestGestureGapsAndInterruptedDwell();
    TestScreenNormalCalibration(); TestCalibrationRejectsMotionAndDiscontinuity(); TestCalibratedRaisePolicy();
    std::puts("Watch activity: 10 test groups passed");
}
