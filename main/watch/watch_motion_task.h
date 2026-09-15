#pragma once

#include "watch_activity.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <pthread.h>
#include <utility>

enum class WatchMotionLoad { kAbsent, kLoaded, kFailed, kBusy, kCorrupt };
struct WatchMotionClock { uint64_t monotonic_ms = 0; std::optional<int32_t> day; };

class WatchMotionTask {
public:
    struct Hooks {
        std::function<bool()> initialize;
        std::function<uint64_t()> start;
        std::function<bool(WatchImuReading&)> read;
        std::function<bool()> stop;
        std::function<WatchMotionLoad(WatchActivityRecord&, bool exclusive_sleep)> load;
        // exclusive_sleep is true only while the power owner awaits Suspend().
        std::function<bool(const WatchActivityRecord&, bool exclusive_sleep)> save;
        std::function<WatchMotionClock()> clock;
        std::function<bool()> recording_or_transfer;
        // The reading is valid only during this callback. Null interrupts a
        // gesture; allowed=false also cancels an in-progress calibration.
        std::function<void(const WatchImuReading*, uint64_t, bool)> motion_observation = {};
    };
    explicit WatchMotionTask(Hooks hooks) : hooks_(std::move(hooks)) {}
    ~WatchMotionTask();
    WatchMotionTask(const WatchMotionTask&) = delete;
    WatchMotionTask& operator=(const WatchMotionTask&) = delete;
    bool Start();
    // No lock is held across peripheral I/O. False means pins must stay available.
    bool Suspend(uint32_t timeout_ms);
    bool Resume(uint32_t timeout_ms);
    bool RetryStorage(uint64_t request_id);
    bool ResetCorruptStorage(uint64_t request_id, uint64_t repair_token);
    WatchMotionSnapshot Snapshot() const;

private:
    static void* Entry(void* self);
    void Run();
    bool Save(WatchActivity& activity, bool sleeping);
    void Publish(const WatchActivity& activity, const WatchMotionClock& clock,
        bool initialized, bool storage_error, bool sensor_error);
    void CompleteStorageRetry(uint64_t request_id, bool succeeded);
    void CompleteStorageReset(uint64_t request_id, bool succeeded);
    void SetRepairable(bool corrupt);
    void ObserveMotion(const WatchImuReading* reading, uint64_t now_ms, bool allowed);
    Hooks hooks_;
    pthread_t thread_{};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool started_ = false;
    bool quit_ = false;
    bool sleep_requested_ = false;
    bool sleep_ready_ = false;
    bool sleep_in_progress_ = false;
    WatchMotionSnapshot snapshot_;
    std::optional<WatchActivityRecord> last_saved_;
    uint64_t repair_generation_ = 0;
    uint64_t reset_token_ = 0;
};
