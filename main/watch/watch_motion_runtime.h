#pragma once

#include "watch_motion_task.h"
#include "watch_qmi8658.h"
#include "watch_wrist_control.h"

class WatchMotionRuntime {
public:
    explicit WatchMotionRuntime(i2c_master_bus_handle_t bus);
    bool Start() { return task_.Start(); }
    bool PrepareForSleep() { return task_.Suspend(2000); }
    bool CancelSleep() { return task_.Resume(2000); }
    bool RetryStorage(uint64_t request_id) { return task_.RetryStorage(request_id); }
    bool ResetCorruptStorage(uint64_t request_id, uint64_t token) { return task_.ResetCorruptStorage(request_id, token); }
    WatchMotionSnapshot Snapshot() const { return task_.Snapshot(); }
    bool RequestWrist(uint64_t id, WatchWristAction action, bool enabled);
    WatchWristSnapshot GetWristSnapshot() const { return wrist_.Snapshot(); }

private:
    WatchQmi8658 sensor_;
    WatchWristControl wrist_;
    void ObserveWrist(const WatchImuReading* reading, uint64_t now_ms, bool allowed);
    // Destroy/join the worker before destroying its sensor or captured board bus.
    WatchMotionTask task_;
};
