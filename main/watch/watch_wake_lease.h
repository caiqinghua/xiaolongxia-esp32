#pragma once

#include <cstdint>
#include <mutex>

// An in-memory permission, never restored from NVS. Only the main settings
// owner may grant it after application and acknowledgement have succeeded.
class WatchWakeLease {
public:
    bool Grant(uint64_t binding_generation, uint64_t audio_generation,
               uint64_t now_ms, uint64_t expires_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        binding_generation_ = audio_generation_ = 0;
        if (!binding_generation || !audio_generation || expires_ms <= now_ms) return false;
        binding_generation_ = binding_generation;
        audio_generation_ = audio_generation;
        granted_ms_ = now_ms;
        expires_ms_ = expires_ms;
        return true;
    }

    bool Allows(uint64_t binding_generation, uint64_t audio_generation, uint64_t now_ms) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return binding_generation_ && audio_generation_ &&
            binding_generation == binding_generation_ && audio_generation == audio_generation_ &&
            now_ms >= granted_ms_ && now_ms < expires_ms_;
    }

    void Revoke() {
        std::lock_guard<std::mutex> lock(mutex_);
        binding_generation_ = audio_generation_ = 0;
    }

private:
    mutable std::mutex mutex_;
    uint64_t binding_generation_ = 0, audio_generation_ = 0;
    uint64_t granted_ms_ = 0, expires_ms_ = 0;
};
