#include "audio/audio_service.h"
#include "watch/watch_level_settings.h"

PlaybackVolumeStatus AudioService::SetLocalPlaybackVolume(uint64_t generation, int volume) {
    std::lock_guard<std::mutex> control(audio_control_mutex_);
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (!IsValidWatchLevel(WatchLevel::kVolume, volume) || !generation || local_playback_generation_ != generation ||
        local_playback_preparation_pending_ || service_stopped_ || audio_output_active_ || !codec_ || !session_coordinator_ ||
        !session_coordinator_->OwnsLocalPlaybackResources(generation) ||
        (session_coordinator_->Mode() != AudioSessionMode::kLocalPlayback &&
         session_coordinator_->Mode() != AudioSessionMode::kPlaybackPaused)) return PlaybackVolumeStatus::kRejected;
    const int previous = ReadWatchLevel(WatchLevel::kVolume);
    if (previous < 0) return PlaybackVolumeStatus::kFailed;
    // The playback worker already holds storage/power through this transaction.
    // A recording preemption must wait for its output cleanup before capture.
    const bool applied = codec_->SetWatchOutputVolume(volume);
    if (applied && SaveWatchLevel(WatchLevel::kVolume, volume)) return PlaybackVolumeStatus::kApplied;
    const bool restored = codec_->SetWatchOutputVolume(previous);
    // A failed commit can leave a staged value visible; restore storage too.
    const bool saved = !applied || SaveWatchLevel(WatchLevel::kVolume, previous);
    return restored && saved ? PlaybackVolumeStatus::kFailed : PlaybackVolumeStatus::kUnsafe;
}

bool AudioService::PrepareLocalPlaybackOutput(uint64_t generation) {
    std::lock_guard<std::mutex> control(audio_control_mutex_);
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        if (service_stopped_ || !codec_ || !codec_->SupportsWatchPlayback() || !session_coordinator_ ||
            !session_coordinator_->OwnsLocalPlaybackPreparation(generation) || local_playback_generation_) return false;
        local_playback_generation_ = generation;
        local_playback_preparation_pending_ = true;
    }
    const bool prepared = QuiesceAssistantLocked(3000, generation);
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    local_playback_preparation_pending_ = !prepared;
    return prepared && session_coordinator_->IsCurrent(generation);
}

bool AudioService::WriteLocalPlaybackOutput(uint64_t generation, const std::vector<int16_t>& pcm) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (!generation || local_playback_generation_ != generation || local_playback_preparation_pending_ || service_stopped_ || !session_coordinator_ ||
        !session_coordinator_->OwnsLocalPlaybackResources(generation) ||
        session_coordinator_->Mode() != AudioSessionMode::kLocalPlayback || audio_output_active_ ||
        pcm.empty() || pcm.size() > static_cast<size_t>(codec_->output_sample_rate()) * 120 / 1000) return false;
    audio_output_active_ = true;
    lock.unlock();
    const bool written = codec_->SetWatchPlaybackOutput(true) && codec_->WriteWatchPlayback(pcm);
    lock.lock();
    audio_output_active_ = false;
    if (written) last_output_time_ = std::chrono::steady_clock::now();
    audio_queue_cv_.notify_all();
    return written;
}

bool AudioService::PauseLocalPlaybackOutput(uint64_t generation) {
    std::lock_guard<std::mutex> control(audio_control_mutex_);
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (!generation || local_playback_generation_ != generation || local_playback_preparation_pending_ || audio_output_active_) return false;
    return codec_->SetWatchPlaybackOutput(false);
}

bool AudioService::CloseLocalPlaybackOutput(uint64_t generation) {
    std::lock_guard<std::mutex> control(audio_control_mutex_);
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (!local_playback_generation_) return true;
    if (!generation || local_playback_generation_ != generation || audio_output_active_) return false;
    if (local_playback_preparation_pending_) {
        lock.unlock();
        const bool drained = QuiesceAssistantLocked(3000, generation);
        lock.lock();
        if (!drained) return false;
        local_playback_preparation_pending_ = false;
    }
    if (!codec_->SetWatchPlaybackOutput(false)) return false;
    local_playback_generation_ = 0;
    return true;
}
