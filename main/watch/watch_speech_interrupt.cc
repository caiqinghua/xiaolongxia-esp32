#include "watch_speech_interrupt.h"

#include <algorithm>

void WatchSpeechInterrupt::ClearLocked() {
    generation_ = 0;
    first_ = count_ = speech_samples_ = 0;
    received_ = pending_ = false;
    // Private pre-trigger speech must not survive cancellation or handoff.
    samples_.fill(0);
}

bool WatchSpeechInterrupt::Begin(uint64_t generation, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!generation || generation_) return false;
    ClearLocked();
    generation_ = generation;
    started_ms_ = last_ms_ = now_ms;
    return true;
}

bool WatchSpeechInterrupt::Feed(uint64_t generation, const int16_t* pcm, size_t count,
                               bool speech, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!generation_ || generation != generation_) return false;
    if (!pcm || !count || count > kMaximumFrameSamples || now_ms < last_ms_ ||
        now_ms < started_ms_) {
        ClearLocked();
        return false;
    }
    if (pending_ && (now_ms - qualified_ms_ > kPendingLifetimeMs ||
        now_ms - last_ms_ > kMaximumGapMs || count > kCapacitySamples - count_)) {
        ClearLocked();
        return false;
    }
    if (received_ && now_ms - last_ms_ > kMaximumGapMs) {
        first_ = count_ = speech_samples_ = 0;
    }
    last_ms_ = now_ms;
    received_ = true;
    if (now_ms - started_ms_ < kWarmupMs) return false;

    const size_t limit = pending_ ? kCapacitySamples : kPrerollSamples;
    for (size_t i = 0; i < count; ++i) {
        if (count_ == limit) {
            first_ = (first_ + 1) % kCapacitySamples;
            --count_;
        }
        samples_[(first_ + count_) % kCapacitySamples] = pcm[i];
        ++count_;
    }
    if (pending_) return false;
    if (!speech) {
        speech_samples_ = 0;
        return false;
    }
    if (!speech_samples_) {
        speech_started_ms_ = now_ms;
        first_speech_samples_ = count;
    }
    speech_samples_ = std::min(kMinimumSpeechSamples, speech_samples_ + count);
    // Wall-clock stalls do not count as speech, and replayed/bursty frames alone
    // cannot qualify an onset. Allow the duration of the first frame only.
    const auto first_frame_allowance_ms = first_speech_samples_ * 1000 / kSampleRate;
    if (speech_samples_ < kMinimumSpeechSamples ||
        now_ms - speech_started_ms_ + first_frame_allowance_ms < 240) return false;
    pending_ = true;
    qualified_ms_ = now_ms;
    return true;
}

bool WatchSpeechInterrupt::Take(uint64_t generation, uint64_t now_ms, std::vector<int16_t>& prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    prefix.clear();
    if (!generation_ || generation != generation_) return false;
    if (!pending_) return false;
    if (now_ms < last_ms_ || now_ms - last_ms_ > kMaximumGapMs ||
        now_ms - qualified_ms_ > kPendingLifetimeMs) {
        ClearLocked();
        return false;
    }
    prefix.reserve(count_);
    for (size_t i = 0; i < count_; ++i) prefix.push_back(samples_[(first_ + i) % kCapacitySamples]);
    ClearLocked();
    return true;
}

void WatchSpeechInterrupt::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    ClearLocked();
}
