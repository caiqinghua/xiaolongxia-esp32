#include "watch/watch_wrist_control.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>
#include <condition_variable>

namespace {
struct Fixture {
    WatchWristPreferences stored;
    bool present = false, fail_load = false, fail_save = false;
    unsigned writes = 0, wakes = 0;
    WatchWristControl::Hooks Hooks() {
        return {
            [this](WatchWristPreferences& value) {
                if (fail_load) return WatchWristLoad::kFailed;
                value = stored;
                return present ? WatchWristLoad::kLoaded : WatchWristLoad::kAbsent;
            },
            [this](const WatchWristPreferences& value) {
                ++writes;
                if (fail_save) return false;
                stored = value; present = true; return true;
            },
            [this]() { ++wakes; },
        };
    }
};
void Calibrate(WatchWristControl& control, uint64_t id, uint64_t start = 1000) {
    assert(control.Request(id, WatchWristAction::kCalibrate));
    WatchImuReading sample{1, 0, 0, 0, -1};
    for (uint64_t now = start; now <= start + 2000; now += 250) control.Poll(&sample, now, true, false);
}
void TestStorageRecord() {
    for (const auto& preferences : {WatchWristPreferences{}, WatchWristPreferences{false, std::array<float, 3>{0, 0, -1}},
        WatchWristPreferences{true, std::array<float, 3>{0.6f, 0, 0.8f}}}) {
        const auto record = EncodeWatchWristPreferences(preferences);
        assert(record);
        WatchWristPreferences decoded;
        assert(DecodeWatchWristPreferences(*record, decoded));
        assert(decoded.enabled == preferences.enabled && decoded.normal.has_value() == preferences.normal.has_value());
        if (decoded.normal) for (size_t i = 0; i < 3; ++i) assert(std::abs((*decoded.normal)[i] - (*preferences.normal)[i]) < 0.0002f);
    }
    assert(!EncodeWatchWristPreferences({true, std::nullopt}));
    assert(!EncodeWatchWristPreferences({true, std::array<float, 3>{0, 0, std::numeric_limits<float>::quiet_NaN()}}));
    for (const size_t index : {size_t(0), size_t(1), size_t(2), size_t(3), size_t(4), size_t(15)}) {
        auto record = *EncodeWatchWristPreferences({}); record[index] = 255;
        WatchWristPreferences sentinel{true, std::array<float, 3>{1, 0, 0}};
        assert(!DecodeWatchWristPreferences(record, sentinel) && sentinel.enabled);
    }
}
void TestCalibrationConsentAndReload() {
    Fixture f;
    WatchWristControl control(f.Hooks());
    assert(!control.Request(1, WatchWristAction::kCalibrate));
    control.Poll(nullptr, 0, true, true);
    assert(control.Snapshot().loaded && !control.Snapshot().enabled);
    assert(!control.Request(1, WatchWristAction::kSetEnabled, true));
    Calibrate(control, 1);
    assert(control.Snapshot().calibrated && !control.Snapshot().enabled && control.Snapshot().succeeded);
    assert(f.present && !f.stored.enabled && f.writes == 1);
    assert(control.Request(2, WatchWristAction::kSetEnabled, true));
    assert(!control.Snapshot().enabled && control.Snapshot().pending);
    control.Poll(nullptr, 3500, true, true);
    assert(control.Snapshot().enabled && f.stored.enabled);
    WatchWristControl rebooted(f.Hooks());
    rebooted.Poll(nullptr, 0, true, true);
    assert(rebooted.Snapshot().enabled && rebooted.Snapshot().calibrated);
    WatchImuReading away{1, 0, 1, 0, 0}, view{1, 0, 0.6f, 0, -0.8f};
    for (uint64_t now = 200; now <= 1200; now += 200) rebooted.Poll(now < 800 ? &away : &view, now, true, true);
    assert(f.wakes == 1);
    assert(rebooted.Request(1, WatchWristAction::kSetEnabled, false));
    assert(!rebooted.Snapshot().enabled);
    rebooted.Poll(nullptr, 1400, true, true);
    assert(!f.stored.enabled && rebooted.Snapshot().succeeded);
}
void TestFailureCancellationAndRevision() {
    Fixture f;
    WatchWristControl control(f.Hooks()); control.Poll(nullptr, 0, true, false);
    assert(control.Request(1, WatchWristAction::kCalibrate));
    WatchImuReading reading{1, 0, 0, 0, 1};
    control.Poll(&reading, 1000, true, false);
    assert(!control.Request(2, WatchWristAction::kCalibrate));
    assert(control.Request(3, WatchWristAction::kCancel));
    assert(!control.Request(2, WatchWristAction::kSetEnabled, true));
    control.Poll(&reading, 1250, true, false);
    assert(control.Snapshot().succeeded && !control.Snapshot().calibrated && !f.stored.normal);
    f.fail_save = true;
    Calibrate(control, 4, 2000);
    assert(control.Snapshot().storage_error && !control.Snapshot().succeeded && !control.Snapshot().enabled);
    f.fail_save = false;
    assert(control.Request(5, WatchWristAction::kCalibrate));
    control.Poll(&reading, 5000, false, false);
    assert(!control.Snapshot().pending && control.Snapshot().calibration == WatchCalibrationStatus::kFailed);
    f.fail_load = true;
    WatchWristControl failed(f.Hooks()); failed.Poll(nullptr, 0, true, false);
    assert(!failed.Snapshot().loaded && !failed.Snapshot().enabled && failed.Snapshot().storage_error);
}
void TestDisableDuringInFlightEnable() {
    Fixture f; f.present = true; f.stored.normal = std::array<float, 3>{0, 0, 1};
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, release = false;
    auto hooks = f.Hooks(); const auto save = hooks.save;
    hooks.save = [&](const WatchWristPreferences& value) {
        if (value.enabled) {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true; changed.notify_all();
            changed.wait(lock, [&]() { return release; });
        }
        return save(value);
    };
    WatchWristControl control(std::move(hooks)); control.Poll(nullptr, 0, true, false);
    assert(control.Request(1, WatchWristAction::kSetEnabled, true));
    std::thread worker([&]() { control.Poll(nullptr, 1000, true, false); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(3), [&]() { return entered; }));
    }
    assert(control.Request(2, WatchWristAction::kSetEnabled, false));
    assert(!control.Snapshot().enabled && control.Snapshot().pending);
    { std::lock_guard<std::mutex> lock(mutex); release = true; }
    changed.notify_all(); worker.join();
    assert(control.Snapshot().request == 2 && control.Snapshot().pending && !control.Snapshot().enabled);
    control.Poll(nullptr, 1250, true, false);
    assert(!f.stored.enabled && control.Snapshot().succeeded && !control.Snapshot().pending);
}
void TestLoadRecovery() {
    Fixture f;
    f.stored = {true, std::array<float, 3>{0, 0, 1}};
    WatchWristControl absent(f.Hooks());
    absent.Poll(nullptr, 0, true, false);
    assert(absent.Snapshot().loaded && !absent.Snapshot().enabled && !absent.Snapshot().calibrated);
    f.present = f.fail_load = true;
    WatchWristControl control(f.Hooks());
    control.Poll(nullptr, 1000, true, false);
    assert(control.Snapshot().storage_error && !control.Snapshot().enabled);
    f.fail_load = false;
    control.Poll(nullptr, 30999, true, false);
    assert(!control.Snapshot().loaded);
    control.Poll(nullptr, 31000, true, false);
    assert(control.Snapshot().loaded && control.Snapshot().enabled && control.Snapshot().calibrated);
    f.stored.normal = std::array<float, 3>{0, 0, 0};
    WatchWristControl corrupt(f.Hooks());
    corrupt.Poll(nullptr, 0, true, false);
    assert(!corrupt.Snapshot().loaded && corrupt.Snapshot().storage_error && !corrupt.Snapshot().enabled);
    assert(corrupt.Request(1, WatchWristAction::kCancel));
    corrupt.Poll(nullptr, 250, false, false);
    assert(corrupt.Snapshot().loaded && corrupt.Snapshot().succeeded && !f.stored.enabled && !f.stored.normal);
}
}
int main() {
    TestStorageRecord(); TestCalibrationConsentAndReload(); TestFailureCancellationAndRevision(); TestDisableDuringInFlightEnable();
    TestLoadRecovery();
    std::puts("Watch wrist control: 5 test groups passed");
}
