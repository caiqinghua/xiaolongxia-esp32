#include "watch/watch_ble_advertising_policy.h"

#include <cassert>
#include <iostream>
#include <limits>

using Action = WatchBleAdvertisingAction;
using Policy = WatchBleAdvertisingPolicy;

static void TestFastThenIdle() {
    Policy policy;
    policy.Begin(100);
    assert(policy.DataConfigured(true, 101) == Action::kStartFast);
    assert(policy.StartCompleted(true, 102) == Action::kNone);
    assert(policy.Poll(30101) == Action::kNone);
    assert(policy.Poll(30102) == Action::kStop);
    assert(policy.StopCompleted(true, 30103) == Action::kStartIdle);
    assert(policy.StartCompleted(true, 30104) == Action::kNone);
    assert(policy.Poll(300000) == Action::kNone);
}

static void TestConnectionCancelsTransition() {
    Policy policy;
    policy.Begin(0);
    assert(policy.DataConfigured(true, 1) == Action::kStartFast);
    policy.Cancel();
    assert(policy.StartCompleted(true, 2) == Action::kNone);
    assert(policy.StopCompleted(true, 3) == Action::kNone);
    assert(policy.Poll(60000) == Action::kNone);
}

static void TestBoundedStartRetries() {
    Policy policy;
    policy.Begin(0);
    assert(policy.DataConfigured(true, 1) == Action::kStartFast);
    assert(policy.StartCompleted(false, 2) == Action::kNone);
    assert(policy.Poll(1001) == Action::kNone);
    assert(policy.Poll(1002) == Action::kStartFast);
    policy.OperationRejected(Action::kStartFast, 1003);
    assert(policy.Poll(2002) == Action::kNone);
    assert(policy.Poll(2003) == Action::kStartFast);
    assert(policy.StartCompleted(false, 2004) == Action::kNone);
    assert(policy.Poll(10000) == Action::kNone);
}

static void TestStopFailureRetriesTransition() {
    Policy policy;
    policy.Begin(0);
    assert(policy.DataConfigured(true, 1) == Action::kStartFast);
    assert(policy.StartCompleted(true, 2) == Action::kNone);
    assert(policy.Poll(30002) == Action::kStop);
    assert(policy.StopCompleted(false, 30003) == Action::kNone);
    assert(policy.Poll(31002) == Action::kNone);
    assert(policy.Poll(31003) == Action::kStop);
    policy.OperationRejected(Action::kStop, 31004);
    assert(policy.Poll(32004) == Action::kStop);
    assert(policy.StopCompleted(false, 32005) == Action::kNone);
    assert(policy.Poll(100000) == Action::kNone);
}

static void TestFailuresAndClockBounds() {
    Policy policy;
    policy.Begin(0);
    assert(policy.DataConfigured(false, 1) == Action::kNone);
    assert(policy.Poll(100000) == Action::kNone);

    policy.Begin(100);
    assert(policy.DataConfigured(true, 101) == Action::kStartFast);
    assert(policy.StartCompleted(true, 102) == Action::kNone);
    assert(policy.Poll(101) == Action::kNone);

    policy.Begin(std::numeric_limits<int64_t>::max() - 10);
    assert(policy.DataConfigured(true, std::numeric_limits<int64_t>::max() - 9) == Action::kStartFast);
    assert(policy.StartCompleted(true, std::numeric_limits<int64_t>::max() - 8) == Action::kNone);
    assert(policy.Poll(std::numeric_limits<int64_t>::max()) == Action::kNone);
}

static void TestIntervals() {
    assert(Policy::kFast.minimum_interval == 160);
    assert(Policy::kFast.maximum_interval == 240);
    assert(Policy::kIdle.minimum_interval == 1600);
    assert(Policy::kIdle.maximum_interval == 2000);
    assert(Policy::kFast.minimum_interval < Policy::kFast.maximum_interval);
    assert(Policy::kIdle.minimum_interval < Policy::kIdle.maximum_interval);
}

int main() {
    TestFastThenIdle();
    TestConnectionCancelsTransition();
    TestBoundedStartRetries();
    TestStopFailureRetriesTransition();
    TestFailuresAndClockBounds();
    TestIntervals();
    std::cout << "PASS: six BLE advertising policy groups\n";
}
