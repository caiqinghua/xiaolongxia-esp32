#include "watch_push_to_talk.h"
#include <atomic>

namespace {
uint64_t Deadline(uint64_t now, uint64_t delay) {
    return now > UINT64_MAX - delay ? UINT64_MAX : now + delay;
}
}
uint64_t WatchPushToTalk::NextToken() {
    static std::atomic<uint64_t> next{0};
    uint64_t token = ++next;
    if (!token) token = ++next;
    return token;
}
bool WatchPushToTalk::Press(uint64_t token, uint64_t now_ms) {
    if (!token) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (token == token_) return false;
    token_ = token;
    started_ms_ = now_ms;
    deadline_ms_ = Deadline(now_ms, kConnectionLimitMs);
    held_ = true;
    captured_ = canceled_ = consumed_ = false;
    return true;
}
void WatchPushToTalk::Release(uint64_t token, bool cancel) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (token != token_ || !token || consumed_) return;
    held_ = false;
    canceled_ |= cancel;
}
void WatchPushToTalk::CancelPending() {
    std::lock_guard<std::mutex> lock(mutex_);
    held_ = false;
    canceled_ = true;
}
void WatchPushToTalk::Retire(uint64_t token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!token || token != token_) return;
    held_ = false;
    canceled_ = consumed_ = true;
}
bool WatchPushToTalk::HeldLocked(uint64_t token, uint64_t now_ms) const {
    return token && token == token_ && held_ && !canceled_ && !consumed_ &&
        now_ms >= started_ms_ && now_ms < deadline_ms_;
}
bool WatchPushToTalk::Held(uint64_t token, uint64_t now_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return HeldLocked(token, now_ms);
}
bool WatchPushToTalk::CaptureReady(uint64_t token, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!HeldLocked(token, now_ms)) return false;
    if (!captured_) {
        captured_ = true;
        deadline_ms_ = Deadline(now_ms, kCaptureLimitMs);
    }
    return true;
}
bool WatchPushToTalk::AllowsInput(uint64_t token, uint64_t now_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return captured_ && HeldLocked(token, now_ms);
}
WatchPressCompletion WatchPushToTalk::Finish(uint64_t token, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!token || token != token_ || consumed_ || HeldLocked(token, now_ms)) return WatchPressCompletion::kNone;
    const bool connection_timeout = held_ && !canceled_ && !captured_ && now_ms >= deadline_ms_;
    held_ = false;
    consumed_ = true;
    if (connection_timeout) return WatchPressCompletion::kTimeout;
    return captured_ && !canceled_ ? WatchPressCompletion::kSend : WatchPressCompletion::kCancel;
}
