#pragma once

#include "audio/audio_session_coordinator.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <pthread.h>
#include <vector>

struct WatchAlertSoundSnapshot {
    bool active = false;
    bool failed = false;
    bool cleanup_failed = false;
    uint64_t generation = 0;
};

// Local output uses the same exclusive playback lease as recorded audio.
// It never opens the microphone, file storage, a decoder or the network.
class WatchAlertSound {
public:
    struct Hooks {
        std::function<bool(uint64_t)> prepare;
        std::function<bool(uint64_t, const std::vector<int16_t>&)> write;
        std::function<bool(uint64_t)> close;
        std::function<void()> released;
    };
    WatchAlertSound(AudioSessionCoordinator& coordinator, Hooks hooks, uint32_t sample_rate);
    ~WatchAlertSound();
    WatchAlertSound(const WatchAlertSound&) = delete;
    WatchAlertSound& operator=(const WatchAlertSound&) = delete;

    // Called by the main loop with monotonic milliseconds and current persisted
    // alert bits. A new event may extend the bounded audible window.
    void Poll(uint8_t alerts, uint64_t now_ms, bool new_event = false);
    void Silence();
    bool StopForRecording(uint64_t generation, uint32_t timeout_ms);
    WatchAlertSoundSnapshot Snapshot() const;

private:
    static void* Entry(void* self);
    void Run();
    bool Cancelled();
    void StopLocked();
    AudioSessionCoordinator& coordinator_;
    Hooks hooks_;
    uint32_t sample_rate_;
    mutable std::mutex mutex_;
    std::mutex poll_mutex_;
    std::condition_variable changed_;
    pthread_t thread_{};
    bool joinable_ = false, destroying_ = false, stop_ = false, retry_cleanup_ = false;
    bool silenced_ = false;
    uint8_t alerts_ = 0;
    uint64_t began_ms_ = 0, attempted_ms_ = 0;
    bool attempted_ = false;
    AudioSessionTransition transition_;
    WatchAlertSoundSnapshot snapshot_;
};
