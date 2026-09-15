#pragma once

#include "watch_activity.h"
#include <functional>
#include <mutex>

struct WatchWristPreferences {
    bool enabled = false;
    std::optional<std::array<float, 3>> normal;
};
using WatchWristRecord = std::array<uint8_t, 16>;
std::optional<WatchWristRecord> EncodeWatchWristPreferences(const WatchWristPreferences& preferences);
bool DecodeWatchWristPreferences(const WatchWristRecord& record, WatchWristPreferences& preferences);
enum class WatchWristLoad { kAbsent, kLoaded, kFailed };
enum class WatchWristAction { kCalibrate, kSetEnabled, kCancel };
struct WatchWristSnapshot {
    bool available = false;
    bool loaded = false;
    bool enabled = false;
    bool calibrated = false;
    bool storage_error = false;
    WatchCalibrationStatus calibration = WatchCalibrationStatus::kIdle;
    uint64_t request = 0;
    bool pending = false;
    bool succeeded = false;
};

// Poll runs on the motion worker. Requests and snapshots never wait for NVS I/O.
class WatchWristControl {
public:
    struct Hooks {
        std::function<WatchWristLoad(WatchWristPreferences&)> load;
        std::function<bool(const WatchWristPreferences&)> save;
        std::function<void()> wake;
    };
    explicit WatchWristControl(Hooks hooks);
    bool Request(uint64_t id, WatchWristAction action, bool enabled = false);
    WatchWristSnapshot Snapshot() const;
    void Poll(const WatchImuReading* reading, uint64_t now_ms, bool allowed, bool display_idle);

private:
    Hooks hooks_;
    mutable std::mutex mutex_;
    WatchWristSnapshot state_;
    WatchWristPreferences saved_;
    WatchScreenCalibration calibration_;
    WatchCalibratedRaiseToWake gesture_;
    WatchWristAction action_ = WatchWristAction::kCancel;
    bool desired_enabled_ = false;
    bool loading_ = false;
    bool load_attempted_ = false;
    uint64_t load_ms_ = 0;
    uint64_t calibration_request_ = 0;
    void ApplyLocked(const WatchWristPreferences& preferences);
};
