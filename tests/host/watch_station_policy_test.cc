#include "watch/watch_station_policy.h"
#include <cassert>
#include <cstdio>

int main() {
    using A = WatchStationAction;
    WatchStationPolicy policy;
    assert(policy.Poll(0, true, false, false, true, false) == A::kNone);
    assert(policy.Poll(89999, true, false, false, true, false) == A::kNone);
    assert(policy.Poll(90000, true, false, true, true, false) == A::kNone);
    assert(policy.Poll(90000, true, false, false, true, true) == A::kNone);
    assert(policy.Poll(90000, true, false, false, true, false) == A::kStop);
    policy.Complete(A::kStop, false, 90000);
    assert(!policy.Suspended());
    assert(policy.Poll(91999, true, false, false, true, false) == A::kNone);
    assert(policy.Poll(92000, true, false, false, true, false) == A::kStop);
    policy.Complete(A::kStop, true, 92000);
    assert(policy.Suspended());
    assert(policy.Poll(100000, true, false, false, false, false) == A::kNone);
    policy.Request(100000);
    assert(policy.Poll(100000, true, false, false, false, false) == A::kStart);
    policy.Complete(A::kStart, true, 100000);
    assert(!policy.Suspended());
    assert(policy.Poll(159999, true, false, false, false, false) == A::kNone);
    assert(policy.Poll(160000, true, false, false, false, false) == A::kStop);
    assert(policy.Failed());
    policy.Complete(A::kStop, true, 160000);
    assert(policy.Poll(220000, true, false, false, false, false) == A::kNone);
    assert(policy.Poll(220000, true, true, false, false, false) == A::kStart);
    std::puts("PASS: bounded association, confirmed suspension, and explicit or continuous reconnect");

    WatchStationPolicy failed;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        assert(failed.Poll(attempt * 2000, true, false, false, false, false) == A::kStart);
        failed.Complete(A::kStart, false, attempt * 2000);
    }
    assert(failed.Failed());
    assert(failed.Poll(4000, true, false, false, false, false) == A::kStop);
    failed.Complete(A::kStop, true, 4000);
    assert(failed.Poll(63999, true, true, false, false, false) == A::kNone);
    failed.Request(10000);
    assert(failed.Poll(10000, true, false, false, false, false) == A::kStart);
    failed.Complete(A::kStart, true, 10000);
    failed.KeepForConversation(100000);
    assert(failed.Poll(129999, true, false, false, true, false) == A::kNone);
    assert(failed.Poll(130000, true, false, false, true, false) == A::kStop);
    failed.CancelDemand();
    assert(failed.Poll(140000, false, true, false, false, false) == A::kNone);
    std::puts("PASS: start retries are bounded, explicit demand clears backoff, and permission dominates mode");
}
