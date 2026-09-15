#include "watch_wifi_gate.h"

void WatchWifiGate::Initialize(bool enabled) {
    std::lock_guard<std::mutex> lock(intent_mutex_);
    enabled_ = enabled;
    station_allowed_ = enabled;
    request_ = 0;
    requested_enabled_ = enabled;
}
void WatchWifiGate::Observe(uint64_t request, bool enabled) {
    if (!request) return;
    std::lock_guard<std::mutex> lock(intent_mutex_);
    request_ = request;
    requested_enabled_ = enabled;
    if (!enabled) { enabled_ = false; station_allowed_ = false; }
}
bool WatchWifiGate::ConfirmEnabled(uint64_t request) {
    std::lock_guard<std::mutex> lock(intent_mutex_);
    if (!request || request != request_ || !requested_enabled_) return false;
    enabled_ = true;
    station_allowed_ = true;
    return true;
}
bool WatchWifiGate::IsCurrent(uint64_t request) const {
    std::lock_guard<std::mutex> lock(intent_mutex_);
    return request && request == request_;
}
bool WatchWifiGate::Run(const std::function<bool()>& operation, bool require_enabled) {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    return operation && !TransferActive() && (!require_enabled || Enabled()) && operation();
}
bool WatchWifiGate::RunStation(const std::function<bool()>& operation) {
    return Run([&]() { return StationAllowed() && operation && operation(); });
}
bool WatchWifiGate::BeginTransfer(const std::function<bool()>& operation) {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    if (!operation || !Enabled() || TransferActive()) return false;
    transfer_active_ = true;
    return operation();
}
bool WatchWifiGate::EndTransfer(const std::function<bool()>& cleanup) {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    if (!TransferActive() || !cleanup || !cleanup()) return false;
    transfer_active_ = false;
    return true;
}
WatchWifiGate& GetWatchWifiGate() {
    static WatchWifiGate gate;
    return gate;
}
