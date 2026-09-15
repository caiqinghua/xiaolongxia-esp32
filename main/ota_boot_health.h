#pragma once

#include <cstdint>

// Owned by the application task. Readiness is local, never a cloud response.
class OtaBootHealth {
public:
    void Begin(bool initialized, int64_t now_ms) {
        if (state_ != State::kDisabled) return;
        state_ = initialized && now_ms >= 0 ? State::kObserving : State::kFailed;
        first_ms_ = last_ms_ = now_ms;
    }

    bool Observe(int64_t now_ms, bool healthy) {
        if (state_ != State::kObserving) return false;
        if (now_ms < 0 || now_ms < last_ms_) {
            state_ = State::kFailed;
            return false;
        }
        if (!healthy || now_ms - last_ms_ > 2000) {
            first_ms_ = last_ms_ = now_ms;
            ticks_ = 0;
            return false;
        }
        if (now_ms == last_ms_) return false;
        last_ms_ = now_ms;
        if (ticks_ < 30) ++ticks_;
        if (ticks_ == 30 && now_ms - first_ms_ >= 30000) {
            state_ = State::kConfirming;
            return true;
        }
        return false;
    }

    void Complete(bool confirmed) {
        if (state_ == State::kConfirming)
            state_ = confirmed ? State::kConfirmed : State::kFailed;
    }

    bool Confirmed() const { return state_ == State::kConfirmed; }
    bool Failed() const { return state_ == State::kFailed; }
private:
    enum class State { kDisabled, kObserving, kConfirming, kConfirmed, kFailed };
    State state_ = State::kDisabled;
    int64_t first_ms_ = 0;
    int64_t last_ms_ = 0;
    unsigned ticks_ = 0;
};
