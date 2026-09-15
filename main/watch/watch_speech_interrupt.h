#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// Accepts only post-AEC mono 16 kHz frames. Feed runs on the AFE worker;
// Begin, Take and Cancel run on the application owner. It never uploads audio.
class WatchSpeechInterrupt {
public:
    static constexpr size_t kSampleRate = 16000;
    static constexpr size_t kMinimumSpeechSamples = kSampleRate * 240 / 1000;
    static constexpr size_t kPrerollSamples = kSampleRate * 400 / 1000;
    static constexpr size_t kCapacitySamples = kSampleRate;
    static constexpr size_t kMaximumFrameSamples = kSampleRate / 10;
    static constexpr uint64_t kWarmupMs = 300;
    static constexpr uint64_t kMaximumGapMs = 200;
    static constexpr uint64_t kPendingLifetimeMs = 500;

    bool Begin(uint64_t generation, uint64_t now_ms);
    // True once per qualified onset; continued frames extend the pinned prefix.
    bool Feed(uint64_t generation, const int16_t* pcm, size_t count, bool speech, uint64_t now_ms);
    bool Take(uint64_t generation, uint64_t now_ms, std::vector<int16_t>& prefix);
    void Cancel();

private:
    void ClearLocked();
    mutable std::mutex mutex_;
    std::array<int16_t, kCapacitySamples> samples_{};
    uint64_t generation_ = 0, started_ms_ = 0, last_ms_ = 0, qualified_ms_ = 0;
    uint64_t speech_started_ms_ = 0;
    size_t first_speech_samples_ = 0;
    size_t first_ = 0, count_ = 0, speech_samples_ = 0;
    bool received_ = false, pending_ = false;
};
