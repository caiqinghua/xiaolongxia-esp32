#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

// Radio operations serialize with stop. Intent callbacks never wait for a
// driver operation; disabling prevents the next operation from starting.
class WatchWifiGate {
public:
    void Initialize(bool enabled);
    void Observe(uint64_t request, bool enabled);
    bool ConfirmEnabled(uint64_t request);
    bool IsCurrent(uint64_t request) const;
    bool Enabled() const { return enabled_.load(); }
    bool TransferActive() const { return transfer_active_.load(); }
    bool StationAllowed() const { return station_allowed_.load(); }
    void SetStationAllowed(bool allowed) { station_allowed_ = allowed; }
    bool RunStation(const std::function<bool()>& operation);
    bool Run(const std::function<bool()>& operation, bool require_enabled = true);
    // Failed starts retain ownership until checked cleanup succeeds.
    bool BeginTransfer(const std::function<bool()>& operation);
    bool EndTransfer(const std::function<bool()>& cleanup);
private:
    std::atomic<bool> enabled_{true};
    std::atomic<bool> station_allowed_{true};
    std::atomic<bool> transfer_active_{false};
    mutable std::mutex intent_mutex_;
    std::recursive_mutex operation_mutex_;
    uint64_t request_ = 0;
    bool requested_enabled_ = true;
};

WatchWifiGate& GetWatchWifiGate();
