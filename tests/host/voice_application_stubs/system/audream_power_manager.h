#pragma once
#include <atomic>
#include <cassert>
enum class AudreamPowerState { kActive, kIdle, kDeepSleepPreparing };
class AudreamPowerManager {
public:
    std::atomic<int> network_leases{0};
    int storage_leases = 0;
    bool allowed = true;
    bool saver = false;
    bool policy_allowed = true;
    bool SetIdleDeepSleepEnabled(bool enabled) {
        if (!policy_allowed || state == AudreamPowerState::kDeepSleepPreparing) return false;
        saver = enabled;
        return true;
    }
    bool IdleDeepSleepEnabled() const { return saver; }
    AudreamPowerState state = AudreamPowerState::kActive;
    AudreamPowerState State() const { return state; }
    static AudreamPowerManager& GetInstance() { static AudreamPowerManager power; return power; }
    bool BeginVoiceNetworkRequest(bool allow_idle = false) {
        if (!allowed || (state != AudreamPowerState::kActive &&
            !(allow_idle && !saver && state == AudreamPowerState::kIdle))) return false;
        ++network_leases;
        return true;
    }
    void EndVoiceNetworkRequest() { assert(network_leases > 0); --network_leases; }
    bool BeginStorageRead() { if (!allowed) return false; ++storage_leases; return true; }
    void EndStorageRead() { assert(storage_leases > 0); --storage_leases; }
};
