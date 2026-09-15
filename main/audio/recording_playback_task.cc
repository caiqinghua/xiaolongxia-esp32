#include "recording_playback_task.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits.h>
#include <thread>

namespace {
void YieldPlaybackScan() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}

RecordingPlaybackTask::~RecordingPlaybackTask() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!joinable_) return;
        stop_ = destroying_ = retry_cleanup_ = true;
        changed_.notify_all();
    }
    // Never destroy hooks while native file/codec/output operations still run.
    if (pthread_join(thread_, nullptr) != 0) std::abort();
}

uint64_t RecordingPlaybackTask::Start(std::string recording_id, std::string path, uint64_t expected_bytes) {
    std::unique_lock<std::mutex> operation(start_mutex_, std::try_to_lock);
    if (!operation.owns_lock()) return 0;
    std::unique_lock<std::mutex> lock(mutex_);
    if (active_ || snapshot_.resources_held || destroying_ || recording_id.empty() || path.empty() ||
        !expected_bytes || !hooks_.acquire_storage || !hooks_.release_storage || !hooks_.prepare_output ||
        !hooks_.write_output || !hooks_.pause_output || !hooks_.close_output) return 0;
    if (joinable_) {
        lock.unlock();
        const int result = pthread_join(thread_, nullptr);
        lock.lock();
        if (result != 0) return 0;
        joinable_ = false;
    }
    transition_ = coordinator_.Begin(AudioSessionMode::kLocalPlayback);
    if (!transition_) return 0;
    snapshot_ = {};
    snapshot_.generation = transition_.generation;
    snapshot_.recording_id = std::move(recording_id);
    snapshot_.sample_rate = sample_rate_;
    snapshot_.volume_supported = static_cast<bool>(hooks_.set_volume);
    snapshot_.state = RecordingPlaybackState::kPreparing;
    snapshot_.resources_held = true;
    path_ = std::move(path);
    expected_bytes_ = expected_bytes;
    stop_ = pause_ = retry_cleanup_ = false;
    active_ = true;
    pthread_attr_t attributes;
    const bool initialized = pthread_attr_init(&attributes) == 0;
    const bool configured = initialized &&
        pthread_attr_setstacksize(&attributes, std::max<size_t>(32768, PTHREAD_STACK_MIN)) == 0;
    const int result = configured ? pthread_create(&thread_, &attributes, Entry, this) : -1;
    if (initialized) pthread_attr_destroy(&attributes);
    if (result != 0) {
        active_ = snapshot_.resources_held = false;
        snapshot_.state = RecordingPlaybackState::kFailed;
        snapshot_.error = RecordingPlaybackError::kPrepare;
        coordinator_.ReleaseLocalPlayback(transition_.generation);
        return 0;
    }
    joinable_ = true;
    return transition_.generation;
}

bool RecordingPlaybackTask::SetPaused(uint64_t generation, bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!generation || generation != snapshot_.generation || !active_ || stop_ ||
        !coordinator_.OwnsLocalPlaybackResources(generation) ||
        (snapshot_.state != RecordingPlaybackState::kPlaying && snapshot_.state != RecordingPlaybackState::kPaused &&
         snapshot_.state != RecordingPlaybackState::kPausing)) return false;
    pause_ = paused;
    if (paused && snapshot_.state == RecordingPlaybackState::kPlaying) snapshot_.state = RecordingPlaybackState::kPausing;
    changed_.notify_all();
    return true;
}

bool RecordingPlaybackTask::SetVolume(uint64_t generation, uint64_t request, int volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!generation || generation != snapshot_.generation || !request || request <= snapshot_.volume.request ||
        volume < 0 || volume > 100 || !hooks_.set_volume || !active_ || stop_ ||
        snapshot_.volume.status == PlaybackVolumeStatus::kPending ||
        !coordinator_.OwnsLocalPlaybackResources(generation) ||
        (snapshot_.state != RecordingPlaybackState::kPlaying && snapshot_.state != RecordingPlaybackState::kPaused)) return false;
    snapshot_.volume = {request, volume, PlaybackVolumeStatus::kPending};
    changed_.notify_all();
    return true;
}

bool RecordingPlaybackTask::Seek(uint64_t generation, uint64_t request, uint64_t target_sample) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!generation || generation != snapshot_.generation || !request || request <= snapshot_.seek.request ||
        !active_ || stop_ || !snapshot_.duration_known || target_sample > snapshot_.total_samples ||
        snapshot_.volume.status == PlaybackVolumeStatus::kPending || snapshot_.seek.status == PlaybackSeekStatus::kPending ||
        !coordinator_.OwnsLocalPlaybackResources(generation) ||
        (snapshot_.state != RecordingPlaybackState::kPlaying && snapshot_.state != RecordingPlaybackState::kPaused)) return false;
    snapshot_.seek = {request, target_sample, PlaybackSeekStatus::kPending};
    snapshot_.state = RecordingPlaybackState::kSeeking;
    changed_.notify_all();
    return true;
}

void RecordingPlaybackTask::SignalStopLocked() {
    stop_ = true;
    if (snapshot_.state == RecordingPlaybackState::kCleanupFailed) retry_cleanup_ = true;
    else if (active_) snapshot_.state = RecordingPlaybackState::kStopping;
    changed_.notify_all();
}

bool RecordingPlaybackTask::RequestStop(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!generation || generation != snapshot_.generation || !active_) return false;
    SignalStopLocked();
    return true;
}

bool RecordingPlaybackTask::WaitStopped(std::unique_lock<std::mutex>& lock, uint32_t timeout_ms) {
    SignalStopLocked();
    return changed_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
        [this] { return !active_ && !snapshot_.resources_held; });
}

bool RecordingPlaybackTask::Stop(uint64_t generation, uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!generation || generation != snapshot_.generation) return false;
    return WaitStopped(lock, timeout_ms);
}

bool RecordingPlaybackTask::StopForRecording(uint64_t new_generation, uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!coordinator_.HoldsExclusiveResources(new_generation) || new_generation == snapshot_.generation) return false;
    return WaitStopped(lock, timeout_ms) && coordinator_.IsCurrent(new_generation);
}

RecordingPlaybackSnapshot RecordingPlaybackTask::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

bool RecordingPlaybackTask::Cancelled() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_ || !coordinator_.IsCurrent(transition_.generation);
}

void RecordingPlaybackTask::Fail(RecordingPlaybackError error, RecordingReadError read_error) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.error = error;
    snapshot_.read_error = read_error;
}

bool RecordingPlaybackTask::SeekOutput(RecordingPlaybackSource& source, RecordingPlaybackDecoder& decoder,
    uint64_t target, std::vector<int16_t>& pcm) {
    pcm.clear();
    if (!hooks_.pause_output(transition_.generation)) { if (!Cancelled()) Fail(RecordingPlaybackError::kOutput); return false; }
    uint64_t raw_start = 0;
    if (!source.Seek(target, raw_start, [this] { return Cancelled(); }, YieldPlaybackScan)) {
        if (!Cancelled()) Fail(RecordingPlaybackError::kRead, source.Error());
        return false;
    }
    if (!decoder.Reset(raw_start)) { Fail(RecordingPlaybackError::kDecode); return false; }
    RecordingOpusPacket packet;
    while (!Cancelled()) {
        const auto status = source.Next(packet, [this] { return Cancelled(); });
        if (status == RecordingReadStatus::kPending) { YieldPlaybackScan(); continue; }
        if (status != RecordingReadStatus::kPacket) {
            if (!Cancelled()) Fail(RecordingPlaybackError::kRead, source.Error());
            return false;
        }
        const auto before = decoder.DecodedSamples();
        if (!decoder.Decode(packet, pcm)) { Fail(RecordingPlaybackError::kDecode); return false; }
        if (packet.eos && target == 0 && source.TotalSamples() == 0 && pcm.empty()) return true;
        const uint64_t first = before > source.PreSkipSamples() ? before - source.PreSkipSamples() : 0;
        if (decoder.DecodedSamples() >= target + source.PreSkipSamples() && first + pcm.size() >= target) {
            if (first > target) { Fail(RecordingPlaybackError::kDecode); return false; }
            pcm.erase(pcm.begin(), pcm.begin() + static_cast<size_t>(target - first));
            return true;
        }
        pcm.clear();
    }
    return false;
}

bool RecordingPlaybackTask::WaitRunnable(RecordingPlaybackSource& source, RecordingPlaybackDecoder& decoder, std::vector<int16_t>& pcm) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_ && coordinator_.IsCurrent(transition_.generation)) {
        if (snapshot_.seek.status == PlaybackSeekStatus::kPending) {
            const auto target = snapshot_.seek.target_sample;
            lock.unlock();
            const bool sought = SeekOutput(source, decoder, target, pcm);
            lock.lock();
            if (stop_ || !coordinator_.IsCurrent(transition_.generation)) {
                snapshot_.seek.status = PlaybackSeekStatus::kCancelled;
                pcm.clear(); return false;
            }
            snapshot_.seek.status = sought ? PlaybackSeekStatus::kApplied : PlaybackSeekStatus::kFailed;
            if (!sought) { pcm.clear(); return false; }
            snapshot_.output_samples = target;
            if (!coordinator_.SetLocalPlaybackPaused(transition_.generation, pause_)) return false;
            snapshot_.state = pause_ ? RecordingPlaybackState::kPaused : RecordingPlaybackState::kPlaying;
            changed_.notify_all();
            continue;
        }
        if (snapshot_.volume.status == PlaybackVolumeStatus::kPending) {
            const int value = snapshot_.volume.value;
            lock.unlock();
            // Only this worker touches playback output; no PCM write overlaps
            // a volume transaction, including while playback is paused.
            auto result = hooks_.set_volume(transition_.generation, value);
            lock.lock();
            if (result != PlaybackVolumeStatus::kApplied && result != PlaybackVolumeStatus::kRejected &&
                result != PlaybackVolumeStatus::kFailed) result = PlaybackVolumeStatus::kUnsafe;
            snapshot_.volume.status = result;
            changed_.notify_all();
            if (result == PlaybackVolumeStatus::kUnsafe) {
                snapshot_.error = RecordingPlaybackError::kOutput;
                return false;
            }
            continue;
        }
        if (!pause_) {
            if (!coordinator_.SetLocalPlaybackPaused(transition_.generation, false)) return false;
            snapshot_.state = RecordingPlaybackState::kPlaying;
            return true;
        }
        if (snapshot_.state != RecordingPlaybackState::kPaused) {
            lock.unlock();
            const bool paused = hooks_.pause_output(transition_.generation);
            lock.lock();
            if (!paused) { snapshot_.error = RecordingPlaybackError::kOutput; return false; }
            if (!coordinator_.SetLocalPlaybackPaused(transition_.generation, true)) return false;
            snapshot_.state = RecordingPlaybackState::kPaused;
            changed_.notify_all();
        }
        // Poll ownership as well: recording may preempt before its stop callback runs.
        changed_.wait_for(lock, std::chrono::milliseconds(20));
    }
    return false;
}

void* RecordingPlaybackTask::Entry(void* self) {
    static_cast<RecordingPlaybackTask*>(self)->Run();
    return nullptr;
}

void RecordingPlaybackTask::Run() {
    RecordingPlaybackSource source;
    RecordingPlaybackDecoder decoder;
    bool storage = false;
    bool output_touched = false;
    bool ended = false;
    do {
        if (Cancelled()) break;
        storage = hooks_.acquire_storage();
        if (!storage) { Fail(RecordingPlaybackError::kStorage); break; }
        if (Cancelled()) break;
        if (!source.Open(path_.c_str(), expected_bytes_, sample_rate_, [this] { return Cancelled(); }, YieldPlaybackScan)) {
            if (!Cancelled()) Fail(RecordingPlaybackError::kRead, source.Error());
            break;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.total_samples = source.TotalSamples();
            snapshot_.duration_known = true;
        }
        if (Cancelled()) break;
        output_touched = true;
        if (!hooks_.prepare_output(transition_.generation)) {
            if (!Cancelled()) Fail(RecordingPlaybackError::kPrepare);
            break;
        }
        if (Cancelled()) break;
        if (!decoder.Open(source.Info(), sample_rate_)) { Fail(RecordingPlaybackError::kDecode); break; }
        if (!coordinator_.Complete(transition_)) break;
        RecordingOpusPacket packet;
        std::vector<int16_t> pcm;
        while (!Cancelled()) {
            if (!WaitRunnable(source, decoder, pcm)) break;
            if (pcm.empty()) {
                const auto status = source.Next(packet, [this] { return Cancelled(); });
                if (status == RecordingReadStatus::kCancelled) break;
                if (status == RecordingReadStatus::kPending) { YieldPlaybackScan(); continue; }
                if (status == RecordingReadStatus::kError) { Fail(RecordingPlaybackError::kRead, source.Error()); break; }
                if (status == RecordingReadStatus::kEnd) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (snapshot_.output_samples == snapshot_.total_samples) {
                        ended = true;
                        snapshot_.end = source.EndKind();
                    } else {
                        snapshot_.error = RecordingPlaybackError::kDecode;
                    }
                    break;
                }
                if (!decoder.Decode(packet, pcm)) { Fail(RecordingPlaybackError::kDecode); break; }
            }
            if (!WaitRunnable(source, decoder, pcm)) break;
            if (!pcm.empty()) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (snapshot_.output_samples > snapshot_.total_samples ||
                        pcm.size() > snapshot_.total_samples - snapshot_.output_samples) {
                        snapshot_.error = RecordingPlaybackError::kDecode;
                        break;
                    }
                }
                if (!hooks_.write_output(transition_.generation, pcm)) {
                    if (!Cancelled()) Fail(RecordingPlaybackError::kOutput);
                    break;
                }
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.output_samples += pcm.size();
                pcm.clear();
            }
        }
    } while (false);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.volume.status == PlaybackVolumeStatus::kPending)
            snapshot_.volume.status = PlaybackVolumeStatus::kCancelled;
        if (snapshot_.seek.status == PlaybackSeekStatus::kPending)
            snapshot_.seek.status = PlaybackSeekStatus::kCancelled;
        changed_.notify_all();
    }
    if (!source.Close()) Fail(RecordingPlaybackError::kRead, RecordingReadError::kIo);
    // Close on the same worker that decodes/writes. A timeout in Stop never
    // closes an in-flight decoder or releases power/storage from another thread.
    while (true) {
        const bool decoder_closed = decoder.Close();
        const bool output_closed = !output_touched || hooks_.close_output(transition_.generation);
        if (decoder_closed && output_closed) break;
        coordinator_.MarkLocalPlaybackFault(transition_.generation);
        std::unique_lock<std::mutex> lock(mutex_);
        snapshot_.state = RecordingPlaybackState::kCleanupFailed;
        snapshot_.error = RecordingPlaybackError::kCleanup;
        changed_.notify_all();
        if (destroying_) std::abort();
        changed_.wait(lock, [this] { return retry_cleanup_ || destroying_; });
        retry_cleanup_ = false;
    }
    if (storage) hooks_.release_storage();
    coordinator_.ReleaseLocalPlayback(transition_.generation);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.resources_held = active_ = false;
        snapshot_.state = snapshot_.error != RecordingPlaybackError::kNone ? RecordingPlaybackState::kFailed :
            (ended && !stop_ ? RecordingPlaybackState::kEnded : RecordingPlaybackState::kStopped);
        changed_.notify_all();
    }
    if (hooks_.released) hooks_.released();
}
