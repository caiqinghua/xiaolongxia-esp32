#include "watch/watch_station_recovery.h"
#include "watch/watch_wifi_gate.h"
#include <cassert>
#include <cstdio>
#include <limits>

using Result = WatchStationRecoveryResult;

int main() {
    AudioSessionCoordinator coordinator;
    WatchStationRecovery recovery;
    int starts = 0;
    auto restore = [&] {
        ++starts;
        assert(coordinator.Mode() == AudioSessionMode::kTransitioning);
        assert(!coordinator.Begin(AudioSessionMode::kLocalRecording));
        assert(!coordinator.Begin(AudioSessionMode::kFileTransfer));
        assert(!coordinator.Begin(AudioSessionMode::kSleeping));
        return true;
    };
    assert(recovery.Poll(0, true, true, coordinator, restore) == Result::kIdle);
    recovery.Request();
    assert(recovery.Poll(0, true, true, coordinator, restore) == Result::kStarted);
    assert(!recovery.Pending() && starts == 1 && coordinator.Mode() == AudioSessionMode::kIdle);
    assert(recovery.Poll(1, true, true, coordinator, restore) == Result::kIdle);
    std::puts("PASS: station recovery reserves native startup and consumes successful requests once");

    for (auto mode : {AudioSessionMode::kLocalRecording, AudioSessionMode::kFileTransfer,
            AudioSessionMode::kAssistantConnecting, AudioSessionMode::kMaintenance, AudioSessionMode::kSleeping}) {
        const auto owner = coordinator.Begin(mode);
        assert(owner);
        recovery.Request();
        assert(recovery.Poll(0, true, true, coordinator, restore) == Result::kDeferred);
        assert(coordinator.Complete(owner));
        assert(recovery.Poll(10000, true, true, coordinator, restore) == Result::kDeferred);
        assert(recovery.Pending() && starts == 1);
        if (mode == AudioSessionMode::kSleeping) assert(coordinator.RecoverToIdle(true));
        else assert(coordinator.Complete(coordinator.Begin(AudioSessionMode::kIdle)));
    }
    assert(recovery.Poll(10001, true, true, coordinator, restore) == Result::kStarted);
    assert(starts == 2);
    std::puts("PASS: preparing and active recording transfer assistant maintenance and sleep defer recovery");

    recovery.Request();
    assert(recovery.Poll(0, true, false, coordinator, restore) == Result::kDeferred);
    assert(recovery.Poll(0, true, true, coordinator, {}) == Result::kDeferred);
    assert(starts == 2 && recovery.Pending());
    assert(recovery.Poll(0, false, false, coordinator, restore) == Result::kCanceled);
    assert(!recovery.Pending());
    assert(recovery.Poll(1, true, true, coordinator, restore) == Result::kIdle);
    recovery.Request();
    recovery.Cancel();
    assert(recovery.Poll(2, true, true, coordinator, restore) == Result::kIdle);
    std::puts("PASS: user disable cancels deferred recovery without reopening on a later enable");

    int failures = 0;
    auto fail = [&] { ++failures; return false; };
    recovery.Request();
    assert(recovery.Poll(100, true, true, coordinator, fail) == Result::kDeferred);
    assert(recovery.Poll(2099, true, true, coordinator, fail) == Result::kDeferred);
    assert(failures == 1 && coordinator.Mode() == AudioSessionMode::kIdle);
    assert(recovery.Poll(2100, true, true, coordinator, fail) == Result::kDeferred);
    assert(recovery.Poll(4100, true, true, coordinator, fail) == Result::kFailed);
    assert(failures == 3 && !recovery.Pending());
    assert(recovery.Poll(9999, true, true, coordinator, fail) == Result::kIdle);
    std::puts("PASS: failed starts release reservations and retry at most three times with backoff");

    recovery.Request();
    assert(recovery.Poll(5000, true, true, coordinator, fail) == Result::kDeferred);
    assert(recovery.Poll(4999, true, true, coordinator, fail) == Result::kDeferred);
    recovery.Request();
    assert(recovery.Poll(5001, true, true, coordinator, restore) == Result::kStarted);
    recovery.Request();
    assert(recovery.Poll(std::numeric_limits<uint64_t>::max() - 100, true, true, coordinator, fail) == Result::kDeferred);
    const int previous_failures = failures;
    assert(recovery.Poll(50, true, true, coordinator, fail) == Result::kDeferred);
    assert(failures == previous_failures);
    std::puts("PASS: a new batch resets retry state and clock regression cannot trigger an early attempt");

    recovery.Request();
    const auto owner = coordinator.Begin(AudioSessionMode::kLocalRecording);
    assert(coordinator.Fail(owner, false));
    assert(recovery.Poll(0, true, true, coordinator, restore) == Result::kDeferred);
    assert(coordinator.Mode() == AudioSessionMode::kFault && recovery.Pending());
    assert(coordinator.RecoverToIdle(true));
    assert(recovery.Poll(1, true, true, coordinator, restore) == Result::kStarted);
    std::puts("PASS: failed recording cleanup keeps station recovery blocked until explicit recovery");

    WatchWifiGate gate;
    recovery.Request();
    bool native_called = false;
    assert(recovery.Poll(0, true, true, coordinator, [&] {
        gate.Observe(1, false);
        return gate.Run([&] { native_called = true; return true; });
    }) == Result::kDeferred);
    assert(!native_called);
    assert(recovery.Poll(1, gate.Enabled(), true, coordinator, restore) == Result::kCanceled);
    std::puts("PASS: radio gate rechecks disable intent after the recovery reservation");
}
