#pragma once

#include <cstdint>
#include <mutex>

// One outstanding profile init per host lifecycle. This does not establish the
// originating lifecycle of SDK callbacks delivered after a full host restart.
class BlufiProfileStartup {
public:
    static constexpr int64_t kTimeoutMs = 60000;

    void Begin(int64_t now_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        started_ms_ = now_ms;
        phase_ = now_ms >= 0 ? Phase::kPending : Phase::kFailed;
    }
    void Stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        phase_ = Phase::kIdle;
    }
    void Fail() {
        std::lock_guard<std::mutex> lock(mutex_);
        phase_ = Phase::kFailed;
    }
    bool Complete(bool success, int64_t now_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        ExpireLocked(now_ms);
        if (phase_ != Phase::kPending) return false;
        phase_ = success ? Phase::kReady : Phase::kFailed;
        return success;
    }
    bool Ready(int64_t now_ms) const {
        std::lock_guard<std::mutex> lock(mutex_);
        ExpireLocked(now_ms);
        return phase_ == Phase::kReady;
    }
    bool Failed(int64_t now_ms) const {
        std::lock_guard<std::mutex> lock(mutex_);
        ExpireLocked(now_ms);
        return phase_ == Phase::kFailed;
    }
private:
    enum class Phase { kIdle, kPending, kReady, kFailed };
    void ExpireLocked(int64_t now_ms) const {
        if (phase_ == Phase::kPending &&
            (now_ms < started_ms_ || now_ms - started_ms_ >= kTimeoutMs)) {
            phase_ = Phase::kFailed;
        }
    }
    // Keep the phase and its original deadline coherent across SDK callbacks and queries.
    mutable std::mutex mutex_;
    mutable Phase phase_ = Phase::kIdle;
    int64_t started_ms_ = 0;
};
