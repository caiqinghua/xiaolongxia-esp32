#pragma once

#include <cstdint>
#include <mutex>

enum class WatchPressCompletion { kNone, kSend, kCancel, kTimeout };

// Input callbacks update intent immediately; the application owner performs
// network and codec operations later. Old releases cannot affect a new press.
class WatchPushToTalk {
public:
    static constexpr uint64_t kConnectionLimitMs = 10000;
    static constexpr uint64_t kCaptureLimitMs = 60000;
    static uint64_t NextToken();
    bool Press(uint64_t token, uint64_t now_ms);
    void Release(uint64_t token, bool cancel = false);
    void Retire(uint64_t token);
    void CancelPending();
    bool Held(uint64_t token, uint64_t now_ms) const;
    bool CaptureReady(uint64_t token, uint64_t now_ms);
    bool AllowsInput(uint64_t token, uint64_t now_ms) const;
    WatchPressCompletion Finish(uint64_t token, uint64_t now_ms);
private:
    mutable std::mutex mutex_;
    uint64_t token_ = 0;
    uint64_t started_ms_ = 0;
    uint64_t deadline_ms_ = 0;
    bool held_ = false;
    bool captured_ = false;
    bool canceled_ = false;
    bool consumed_ = false;
    bool HeldLocked(uint64_t token, uint64_t now_ms) const;
};
