#include "watch_voice_capture.h"

bool WatchVoiceCapture::Begin(uint64_t generation, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!generation || generation_) return false;
    generation_ = generation;
    started_ms_ = last_event_ms_ = silence_started_ms_ = now_ms;
    speech_started_ms_ = speech_ms_ = 0;
    speaking_ = false;
    return true;
}

bool WatchVoiceCapture::SeedSpeech(uint64_t generation, uint64_t speech_ms, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!generation_ || generation != generation_ || speaking_ || speech_ms_ ||
        now_ms != started_ms_ || speech_ms < kMinimumSpeechMs || speech_ms > 1000) return false;
    speech_ms_ = speech_ms;
    return true;
}

WatchVoiceCaptureResult WatchVoiceCapture::Evaluate(uint64_t generation, uint64_t now_ms) const {
    if (!generation_ || generation != generation_) return WatchVoiceCaptureResult::kIdle;
    if (now_ms < last_event_ms_ || now_ms < started_ms_) return WatchVoiceCaptureResult::kCancel;
    const uint64_t voiced = speech_ms_ + (speaking_ ? now_ms - speech_started_ms_ : 0);
    if (now_ms - started_ms_ >= kMaximumCaptureMs)
        return voiced >= kMinimumSpeechMs ? WatchVoiceCaptureResult::kSend : WatchVoiceCaptureResult::kCancel;
    if (voiced < kMinimumSpeechMs && now_ms - started_ms_ >= kOnsetTimeoutMs)
        return WatchVoiceCaptureResult::kCancel;
    if (!speaking_ && voiced >= kMinimumSpeechMs && now_ms - silence_started_ms_ >= kSilenceMs)
        return WatchVoiceCaptureResult::kSend;
    return WatchVoiceCaptureResult::kListening;
}

void WatchVoiceCapture::Vad(uint64_t generation, bool speaking, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    // A late onset must not reopen a capture whose silence/deadline already ended.
    if (Evaluate(generation, now_ms) != WatchVoiceCaptureResult::kListening) return;
    last_event_ms_ = now_ms;
    if (speaking == speaking_) return;
    if (speaking) speech_started_ms_ = now_ms;
    else {
        speech_ms_ += now_ms - speech_started_ms_;
        silence_started_ms_ = now_ms;
    }
    speaking_ = speaking;
}

bool WatchVoiceCapture::AllowsInput(uint64_t generation, uint64_t now_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Evaluate(generation, now_ms) == WatchVoiceCaptureResult::kListening;
}

WatchVoiceCaptureResult WatchVoiceCapture::Poll(uint64_t generation, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto result = Evaluate(generation, now_ms);
    if (result == WatchVoiceCaptureResult::kSend || result == WatchVoiceCaptureResult::kCancel)
        generation_ = 0;
    return result;
}

void WatchVoiceCapture::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    generation_ = 0;
}
