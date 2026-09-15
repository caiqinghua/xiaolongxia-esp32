#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

enum class WatchMotionStatus { kUnavailable, kRunning, kPaused, kFault };

struct WatchImuReading {
    uint64_t generation = 0;
    uint32_t steps = 0;
    float x_g = 0;
    float y_g = 0;
    float z_g = 0;
};

struct WatchActivityDay {
    int32_t day = 0;
    uint32_t steps = 0;
};

struct WatchActivityCheckpoint {
    static constexpr size_t kDays = 7;
    std::array<WatchActivityDay, kDays> days{};
    size_t count = 0;
    uint32_t undated_steps = 0;
    bool incomplete = false;
};

using WatchActivityRecord = std::array<uint8_t, 64>;
std::optional<WatchActivityRecord> EncodeWatchActivityCheckpoint(const WatchActivityCheckpoint& checkpoint);
bool DecodeWatchActivityCheckpoint(const uint8_t* data, size_t size, WatchActivityCheckpoint& checkpoint);

struct WatchActivitySnapshot {
    WatchMotionStatus status = WatchMotionStatus::kUnavailable;
    std::optional<int32_t> day;
    uint32_t steps = 0;
    uint32_t undated_steps = 0;
    bool incomplete = false;
};

struct WatchMotionSnapshot {
    WatchActivitySnapshot activity;
    bool initialized = false;
    bool storage_error = false;
    WatchActivityCheckpoint history;
    uint64_t retry_request = 0;
    bool retry_pending = false;
    bool retry_succeeded = false;
    uint64_t repair_token = 0;
    uint64_t reset_request = 0;
    bool reset_pending = false;
    bool reset_succeeded = false;
};

// Single owner: the motion worker. No I/O, cloud upload, or synthetic step detection.
class WatchActivity {
public:
    static constexpr uint32_t kCounterModulus = 1U << 24;
    static constexpr uint64_t kMaximumObservationGapMs = 30000;

    bool Restore(const WatchActivityCheckpoint& checkpoint);
    WatchActivityCheckpoint Checkpoint() const { return saved_; }
    // Called immediately after the hardware counter is confirmed reset to zero.
    bool BeginCounter(uint64_t generation, uint64_t now_ms, std::optional<int32_t> day);
    bool Observe(uint64_t generation, uint32_t counter, uint64_t now_ms,
        std::optional<int32_t> day);
    void ReadFailed();
    void Pause();
    WatchActivitySnapshot Snapshot(std::optional<int32_t> day) const;

private:
    WatchActivityCheckpoint saved_;
    WatchMotionStatus status_ = WatchMotionStatus::kUnavailable;
    uint64_t generation_ = 0;
    uint64_t last_ms_ = 0;
    uint32_t counter_ = 0;
    std::optional<int32_t> day_;
    bool counter_active_ = false;
    void Add(uint32_t steps, std::optional<int32_t> day);
};

// The board supplies calibrated screen-normal acceleration in g. This detects
// an away-to-view gesture, not arbitrary motion, and never configures a wake IRQ.
class WatchRaiseToWake {
public:
    bool Observe(uint64_t now_ms, float screen_up_g, float magnitude_g, bool allowed);
    void Reset();

private:
    enum class Phase { kIdle, kAway, kArmed, kViewing };
    Phase phase_ = Phase::kIdle;
    uint64_t phase_ms_ = 0;
    uint64_t armed_ms_ = 0;
    uint64_t last_ms_ = 0;
    uint64_t wake_ms_ = 0;
    bool sampled_ = false;
    bool woke_ = false;
};

enum class WatchCalibrationStatus { kIdle, kCollecting, kReady, kFailed };

// The user places the display face up on a level surface. No board-axis guess
// or single noisy sample is accepted as the screen normal.
class WatchScreenCalibration {
public:
    void Begin(uint64_t now_ms);
    void Cancel();
    void Observe(const WatchImuReading* reading, uint64_t now_ms, bool allowed);
    WatchCalibrationStatus Status() const { return status_; }
    std::optional<std::array<float, 3>> Normal() const;

private:
    WatchCalibrationStatus status_ = WatchCalibrationStatus::kIdle;
    uint64_t started_ms_ = 0;
    uint64_t window_ms_ = 0;
    uint64_t last_ms_ = 0;
    uint64_t generation_ = 0;
    unsigned samples_ = 0;
    std::array<float, 3> first_{};
    std::array<float, 3> sum_{};
    std::array<float, 3> normal_{};
};

class WatchCalibratedRaiseToWake {
public:
    bool SetScreenNormal(const std::array<float, 3>& normal);
    bool SetEnabled(bool enabled);
    bool Enabled() const { return enabled_; }
    bool Calibrated() const { return calibrated_; }
    bool Observe(const WatchImuReading* reading, uint64_t now_ms, bool allowed);

private:
    WatchRaiseToWake gesture_;
    std::array<float, 3> normal_{};
    uint64_t generation_ = 0;
    bool calibrated_ = false;
    bool enabled_ = false;
};
