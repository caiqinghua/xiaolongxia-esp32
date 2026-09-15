#pragma once

#include <cstdint>
#include <limits>
#include <mutex>

// Button and timer callbacks confirm intent; the application task consumes it.
// Releasing a confirmed hold must not cancel queued recording work.
class RecordingButtonIntent {
public:
    explicit RecordingButtonIntent(int64_t hold_us) : hold_us_(hold_us) {}

    void Press(int64_t now_us) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pressed_ || now_us < 0) return;
        pressed_ = true;
        confirmed_ = false;
        pressed_at_us_ = now_us;
    }

    uint64_t ConfirmHold() {
        std::lock_guard<std::mutex> lock(mutex_);
        return ConfirmLocked();
    }

    uint64_t ConfirmElapsed(int64_t now_us) {
        std::lock_guard<std::mutex> lock(mutex_);
        return HasElapsedLocked(now_us) ? ConfirmLocked() : 0;
    }

    uint64_t Release(int64_t now_us) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto token = HasElapsedLocked(now_us) ? ConfirmLocked() : 0;
        pressed_ = false;
        return token;
    }

    void CancelPress() {
        std::lock_guard<std::mutex> lock(mutex_);
        pressed_ = false;
    }

    bool LastPressWasHold() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return confirmed_;
    }

    bool Consume(uint64_t token) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (token == 0 || token > issued_ || token <= consumed_) return false;
        consumed_ = token;
        return true;
    }

private:
    bool HasElapsedLocked(int64_t now_us) const {
        return pressed_ && hold_us_ > 0 && now_us >= pressed_at_us_ &&
               now_us - pressed_at_us_ >= hold_us_;
    }

    uint64_t ConfirmLocked() {
        if (!pressed_ || confirmed_ || hold_us_ <= 0 ||
            issued_ == std::numeric_limits<uint64_t>::max()) return 0;
        confirmed_ = true;
        return ++issued_;
    }

    const int64_t hold_us_;
    mutable std::mutex mutex_;
    bool pressed_ = false;
    bool confirmed_ = false;
    int64_t pressed_at_us_ = 0;
    uint64_t issued_ = 0;
    uint64_t consumed_ = 0;
};
