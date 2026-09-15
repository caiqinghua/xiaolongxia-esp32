#include "watch/watch_ble_link_policy.h"
#include <cassert>
#include <iostream>
#include <limits>
#include <vector>

using Policy = WatchBleLinkPolicy;
constexpr uint8_t peer[]{1, 2, 3, 4, 5, 6};
constexpr uint8_t other[]{6, 5, 4, 3, 2, 1};
struct Fixture {
    Policy policy;
    bool accepted = true;
    std::vector<WatchBleLinkParameters> requests;
    void Poll(int64_t now, bool busy = false) {
        policy.Poll(now, busy, [&](const auto& address, const auto& params) {
            assert(std::equal(address.begin(), address.end(), peer));
            requests.push_back(params);
            return accepted;
        });
    }
    void Complete(int64_t now, bool success = true) {
        const auto& request = requests.back();
        assert(policy.Updated(peer, request.minimum_interval, request.maximum_interval, success, now));
    }
};

static void TestIdleAndActivity() {
    Fixture f;
    f.Poll(0); assert(f.requests.empty());
    f.policy.Connected(peer, 0);
    f.Poll(4999); assert(f.requests.empty());
    f.Poll(5000); assert(f.requests.size() == 1 && f.requests.back().latency == 0);
    f.Complete(5001);
    f.Poll(14999); assert(f.requests.size() == 1);
    f.Poll(15000); assert(f.requests.size() == 2 && f.requests.back().latency == 4);
    f.Complete(15001);
    f.Poll(30000); assert(f.requests.size() == 2);
    f.policy.Activity(30001);
    f.Poll(30001); assert(f.requests.size() == 3 && f.requests.back().latency == 0);
    f.Complete(30002);
    f.Poll(50000, true); assert(f.requests.size() == 3);
    f.Poll(64999); assert(f.requests.size() == 3);
    f.Poll(65000); assert(f.requests.size() == 4 && f.requests.back().latency == 4);
}

static void TestBoundedRejectionAndTimeout() {
    for (int failure = 0; failure < 3; ++failure) {
        Fixture f; f.policy.Connected(peer, 0);
        f.accepted = failure != 0;
        for (int64_t now = 5000; now <= 120000; now += 1000) {
            const auto before = f.requests.size();
            f.Poll(now, true);
            if (failure == 1 && before != f.requests.size()) f.Complete(now, false);
        }
        assert(f.requests.size() == Policy::kMaxAttempts);
    }
}

static void TestPendingSwitchAndStaleCompletion() {
    Fixture f; f.policy.Connected(peer, 0); f.Poll(5000);
    assert(!f.policy.Updated(other, 24, 36, true, 5001));
    assert(!f.policy.Updated(peer, 36, 48, true, 5001));
    assert(!f.policy.Updated(peer, 0, 0, true, 5001));
    f.Complete(5002);
    f.Poll(15000); assert(f.requests.size() == 2);
    f.policy.Activity(15001);
    f.Poll(15001); assert(f.requests.size() == 2);
    f.Complete(15002);
    f.Poll(20002); assert(f.requests.size() == 3 && f.requests.back().latency == 0);
    assert(!f.policy.Updated(peer, 36, 48, true, 20003));
    f.Complete(20004);
    f.policy.Disconnected(); f.Poll(50000);
    assert(!f.policy.Updated(peer, 24, 36, true, 50001));
    f.policy.Connected(peer, 60000); f.Poll(65000);
    assert(f.requests.size() == 4 && f.requests.back().latency == 0);
}

static void TestClockAndNoDuplicateRequests() {
    Fixture f; f.policy.Connected(peer, 1000); f.Poll(6000);
    f.Complete(6001);
    f.policy.Activity(5999); f.Poll(5999);
    f.Poll(16000); assert(f.requests.size() == 2 && f.requests.back().latency == 4);
    f.Complete(16001);
    for (int n = 0; n < 100; ++n) f.Poll(30000);
    assert(f.requests.size() == 2);
    f.Poll(std::numeric_limits<int64_t>::max());
    f.policy.Activity(40000); f.Poll(50000); assert(f.requests.size() == 2);
    f.policy.Connected(peer, -1); f.Poll(50000); assert(f.requests.size() == 2);
}

static void TestParameters() {
    for (auto p : {Policy::kActive, Policy::kIdle}) {
        assert(p.minimum_interval >= 12 && p.minimum_interval % 12 == 0);
        assert(p.maximum_interval >= p.minimum_interval + 12);
        assert(p.maximum_interval * 5 * (p.latency + 1) <= 8000);
        assert(p.maximum_interval * 5 * (p.latency + 1) * 3 < p.supervision_timeout * 40);
        assert(p.latency <= 30 && p.supervision_timeout >= 200 && p.supervision_timeout <= 600);
    }
}

int main() {
    TestIdleAndActivity(); TestBoundedRejectionAndTimeout(); TestPendingSwitchAndStaleCompletion();
    TestClockAndNoDuplicateRequests(); TestParameters();
    std::cout << "PASS: five BLE link policy groups\n";
}
