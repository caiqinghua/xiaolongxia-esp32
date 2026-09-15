#include "watch/watch_wake_lease.h"
#include <cassert>
#include <thread>
#include <iostream>

int main() {
    WatchWakeLease lease;
    assert(!lease.Allows(1, 2, 100));
    assert(!lease.Grant(0, 2, 100, 200));
    assert(!lease.Grant(1, 0, 100, 200));
    assert(!lease.Grant(1, 2, 100, 100));
    assert(lease.Grant(1, 2, 100, 200));
    assert(!lease.Allows(1, 2, 99));
    assert(lease.Allows(1, 2, 100));
    assert(lease.Allows(1, 2, 199));
    assert(!lease.Allows(1, 2, 200));
    assert(!lease.Allows(0, 2, 110));
    assert(!lease.Allows(3, 2, 110));
    assert(!lease.Allows(1, 3, 110));
    std::thread revoke([&] { lease.Revoke(); });
    revoke.join();
    assert(!lease.Allows(1, 2, 110));
    assert(lease.Grant(3, 4, 110, 200));
    assert(!lease.Allows(1, 2, 111));
    assert(lease.Allows(3, 4, 111));
    assert(!lease.Grant(3, 4, 200, 150));
    assert(!lease.Allows(3, 4, 111));
    std::cout << "PASS wake lease: default deny, expiry, binding, audio ownership, revocation and failed replacement\n";
}
