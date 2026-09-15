#pragma once

#include <cstdint>
#include <mutex>

struct WatchBleAdvertisingParameters {
    uint16_t minimum_interval;
    uint16_t maximum_interval;
};

enum class WatchBleAdvertisingAction : uint8_t {
    kNone,
    kStartFast,
    kStartIdle,
    kStop,
};

// Advertising intervals use 0.625 ms units. The policy keeps discovery fast
// after startup or disconnect, then lowers the continuous standby duty cycle.
class WatchBleAdvertisingPolicy {
public:
    static constexpr WatchBleAdvertisingParameters kFast{160, 240};
    static constexpr WatchBleAdvertisingParameters kIdle{1600, 2000};
    static constexpr int64_t kFastDurationMs = 30000;
    static constexpr int64_t kRetryMs = 1000;
    static constexpr unsigned kMaxStartAttempts = 3;
    static constexpr unsigned kMaxStopAttempts = 3;

    void Begin(int64_t now_ms);
    void Cancel();
    WatchBleAdvertisingAction DataConfigured(bool success, int64_t now_ms);
    WatchBleAdvertisingAction StartCompleted(bool success, int64_t now_ms);
    WatchBleAdvertisingAction StopCompleted(bool success, int64_t now_ms);
    WatchBleAdvertisingAction OperationRejected(WatchBleAdvertisingAction action, int64_t now_ms);
    WatchBleAdvertisingAction Poll(int64_t now_ms);

private:
    enum class State : uint8_t {
        kStopped,
        kAwaitingData,
        kStartingFast,
        kFast,
        kStoppingFast,
        kStartingIdle,
        kIdle,
        kRetryFast,
        kRetryIdle,
    };

    bool CheckClock(int64_t now_ms);
    WatchBleAdvertisingAction ScheduleStart(bool fast, int64_t now_ms);
    void Reset();

    std::mutex mutex_;
    State state_ = State::kStopped;
    int64_t last_ms_ = 0;
    int64_t deadline_ms_ = 0;
    unsigned start_attempts_ = 0;
    unsigned stop_attempts_ = 0;
};
