#pragma once

#include "audio_session_coordinator.h"
#include "recording_playback_decoder.h"
#include "playback_volume.h"
#include "recording_playback_source.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <pthread.h>
#include <string>
#include <utility>

enum class RecordingPlaybackState {
    kIdle, kPreparing, kPlaying, kPausing, kPaused, kStopping,
    kEnded, kStopped, kFailed, kCleanupFailed, kSeeking,
};
enum class PlaybackSeekStatus { kNone, kPending, kApplied, kCancelled, kFailed };
struct PlaybackSeekSnapshot {
    uint64_t request = 0;
    uint64_t target_sample = 0;
    PlaybackSeekStatus status = PlaybackSeekStatus::kNone;
};
enum class RecordingPlaybackError { kNone, kStorage, kRead, kPrepare, kDecode, kOutput, kCleanup };

struct RecordingPlaybackSnapshot {
    RecordingPlaybackState state = RecordingPlaybackState::kIdle;
    RecordingPlaybackError error = RecordingPlaybackError::kNone;
    RecordingReadError read_error = RecordingReadError::kNone;
    RecordingEndKind end = RecordingEndKind::kNone;
    std::string recording_id;
    uint64_t generation = 0;
    uint64_t output_samples = 0;
    uint64_t total_samples = 0;
    bool duration_known = false;
    uint32_t sample_rate = 0;
    bool resources_held = false;
    bool volume_supported = false;
    PlaybackVolumeSnapshot volume;
    PlaybackSeekSnapshot seek;
};

class RecordingPlaybackTask {
public:
    struct Hooks {
        std::function<bool()> acquire_storage;
        std::function<void()> release_storage;
        std::function<bool(uint64_t)> prepare_output;
        std::function<bool(uint64_t, const std::vector<int16_t>&)> write_output;
        std::function<bool(uint64_t)> pause_output;
        std::function<bool(uint64_t)> close_output;
        std::function<void()> released;
        std::function<PlaybackVolumeStatus(uint64_t, int)> set_volume;
    };
    RecordingPlaybackTask(AudioSessionCoordinator& coordinator, Hooks hooks, uint32_t sample_rate)
        : coordinator_(coordinator), hooks_(std::move(hooks)), sample_rate_(sample_rate) {}
    ~RecordingPlaybackTask();
    RecordingPlaybackTask(const RecordingPlaybackTask&) = delete;
    RecordingPlaybackTask& operator=(const RecordingPlaybackTask&) = delete;

    // Caller validates catalog revision, safe recording ID and path before Start.
    uint64_t Start(std::string recording_id, std::string path, uint64_t expected_bytes);
    bool SetPaused(uint64_t generation, bool paused);
    // Accepted requests complete asynchronously in Snapshot().volume.
    bool SetVolume(uint64_t generation, uint64_t request, int volume);
    bool Seek(uint64_t generation, uint64_t request, uint64_t target_sample);
    bool RequestStop(uint64_t generation);
    bool Stop(uint64_t generation, uint32_t timeout_ms);
    bool StopForRecording(uint64_t new_generation, uint32_t timeout_ms);
    RecordingPlaybackSnapshot Snapshot() const;

private:
    static void* Entry(void* self);
    void Run();
    bool Cancelled();
    bool WaitRunnable(RecordingPlaybackSource& source, RecordingPlaybackDecoder& decoder, std::vector<int16_t>& pcm);
    bool SeekOutput(RecordingPlaybackSource& source, RecordingPlaybackDecoder& decoder, uint64_t target, std::vector<int16_t>& pcm);
    bool WaitStopped(std::unique_lock<std::mutex>& lock, uint32_t timeout_ms);
    void SignalStopLocked();
    void Fail(RecordingPlaybackError error, RecordingReadError read_error = RecordingReadError::kNone);
    AudioSessionCoordinator& coordinator_;
    Hooks hooks_;
    uint32_t sample_rate_;
    mutable std::mutex mutex_;
    std::mutex start_mutex_;
    std::condition_variable changed_;
    pthread_t thread_{};
    bool joinable_ = false;
    bool active_ = false;
    bool stop_ = false;
    bool pause_ = false;
    bool retry_cleanup_ = false;
    bool destroying_ = false;
    AudioSessionTransition transition_;
    std::string path_;
    uint64_t expected_bytes_ = 0;
    RecordingPlaybackSnapshot snapshot_;
};
