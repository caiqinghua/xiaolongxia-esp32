#pragma once

#include <cstdint>
#include <mutex>

enum class WatchVoiceCaptureResult { kIdle, kListening, kSend, kCancel };

// One locally delimited utterance after wake detection. No raw audio is stored
// here. The application still uses the existing explicit listen start/stop wire.
class WatchVoiceCapture {
public:
    static constexpr uint64_t kOnsetTimeoutMs = 8000;
    static constexpr uint64_t kSilenceMs = 900;
    static constexpr uint64_t kMinimumSpeechMs = 150;
    static constexpr uint64_t kMaximumCaptureMs = 60000;
    bool Begin(uint64_t generation, uint64_t now_ms);
    bool SeedSpeech(uint64_t generation, uint64_t speech_ms, uint64_t now_ms);
    void Vad(uint64_t generation, bool speaking, uint64_t now_ms);
    bool AllowsInput(uint64_t generation, uint64_t now_ms) const;
    WatchVoiceCaptureResult Poll(uint64_t generation, uint64_t now_ms);
    void Cancel();

private:
    WatchVoiceCaptureResult Evaluate(uint64_t generation, uint64_t now_ms) const;
    mutable std::mutex mutex_;
    uint64_t generation_ = 0, started_ms_ = 0, last_event_ms_ = 0;
    uint64_t speech_started_ms_ = 0, silence_started_ms_ = 0, speech_ms_ = 0;
    bool speaking_ = false;
};
