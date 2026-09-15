#pragma once
#include <atomic>
#include <cassert>
class Blufi {
public:
    static Blufi& GetInstance() { static Blufi instance; return instance; }
    std::atomic<bool> active{true}, provisioning{false};
    bool profile_ready = true;
    bool IsActive() const { return active.load(); }
    bool IsControlReady() const { return active && !provisioning && profile_ready; }
    bool IsWifiProvisioning() const { return provisioning.load(); }
    uint64_t lease = 0, next_lease = 0;
    bool lease_allowed = true;
    uint64_t AcquireControlLease() {
        if (!lease_allowed || lease || !active || provisioning) return 0;
        return lease = ++next_lease;
    }
    bool ReleaseControlLease(uint64_t value) {
        assert(value && value == lease); lease = 0; return true;
    }
};
