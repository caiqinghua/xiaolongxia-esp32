// Compile the real AudioService declaration and local output methods. Only
// service startup/quiescence and codec hardware are controlled host boundaries.
#include "audio/audio_service.h"
#include "watch/watch_level_settings.h"
#include <cassert>
#include <cstdio>
#include <thread>

namespace {
bool quiesce_success = true;
unsigned quiesce_calls = 0;
int saved_volume = 70;
int save_failures = 0;
unsigned save_calls = 0;
class OutputCodec : public AudioCodec {
public:
    OutputCodec() { output_sample_rate_ = 16000; }
    bool supported = true, enable_success = true, write_success = true, close_success = true;
    unsigned closes = 0, writes = 0;
    std::mutex mutex;
    std::condition_variable changed;
    bool block = false, writing = false;
    int volume_failures = 0;
    std::vector<int> volume_attempts;
    bool SetWatchOutputVolume(int volume) override {
        volume_attempts.push_back(volume);
        output_volume_ = volume;
        if (volume_failures) { --volume_failures; return false; }
        return true;
    }
    bool SupportsWatchPlayback() const override { return supported; }
    bool SetWatchPlaybackOutput(bool enabled) override {
        if (!enabled) ++closes;
        const bool success = enabled ? enable_success : close_success;
        if (success) output_enabled_ = enabled;
        return success;
    }
    bool WriteWatchPlayback(const std::vector<int16_t>&) override {
        std::unique_lock<std::mutex> lock(mutex);
        ++writes; writing = true; changed.notify_all();
        changed.wait(lock, [this] { return !block; });
        writing = false;
        return write_success;
    }
    int Read(int16_t*, int) override { assert(false); return 0; }
    int Write(const int16_t*, int) override { assert(false); return 0; }
};
}

bool IsValidWatchLevel(WatchLevel level, int value) {
    assert(level == WatchLevel::kVolume);
    return value >= 0 && value <= 100;
}
int ReadWatchLevel(WatchLevel level) { assert(level == WatchLevel::kVolume); return saved_volume; }
bool SaveWatchLevel(WatchLevel level, int value) {
    assert(level == WatchLevel::kVolume);
    ++save_calls;
    saved_volume = value;
    if (save_failures) { --save_failures; return false; }
    return true;
}

AudioCodec::AudioCodec() = default;
AudioCodec::~AudioCodec() = default;
void AudioCodec::EnableOutput(bool enabled) { output_enabled_ = enabled; }
void AudioCodec::EnableInput(bool enabled) { input_enabled_ = enabled; }
void AudioCodec::SetOutputVolume(int volume) { output_volume_ = volume; }
void AudioCodec::SetInputGain(float gain) { input_gain_ = gain; }
void AudioCodec::PrepareForLocalRecording() {}
void AudioCodec::PrepareForDeepSleep() {}
void AudioCodec::OutputData(std::vector<int16_t>&) { assert(false); }
bool AudioCodec::InputData(std::vector<int16_t>&) { assert(false); return false; }
void AudioCodec::Start() {}
AudioDebugger::~AudioDebugger() = default;
AudioService::AudioService() = default;
AudioService::~AudioService() = default;
void AudioService::Initialize(AudioCodec* codec) { codec_ = codec; service_stopped_ = false; }
void AudioService::SetSessionCoordinator(AudioSessionCoordinator* value) { session_coordinator_ = value; }
bool AudioService::QuiesceAssistantLocked(uint32_t timeout, uint64_t generation) {
    assert(timeout == 3000 && generation == local_playback_generation_);
    ++quiesce_calls; return quiesce_success;
}

int main() {
    using Volume = PlaybackVolumeStatus;
    const std::vector<int16_t> pcm(320, 42);
    AudioSessionCoordinator coordinator;
    OutputCodec codec;
    AudioService audio;
    audio.SetSessionCoordinator(&coordinator);
    const auto initial = coordinator.Begin(AudioSessionMode::kLocalPlayback);
    assert(!audio.PrepareLocalPlaybackOutput(initial.generation));
    audio.Initialize(&codec);
    codec.supported = false;
    assert(!audio.PrepareLocalPlaybackOutput(initial.generation));
    codec.supported = true;
    quiesce_success = false;
    assert(!audio.PrepareLocalPlaybackOutput(initial.generation));
    assert(!audio.CloseLocalPlaybackOutput(initial.generation));
    assert(!audio.PrepareLocalPlaybackOutput(initial.generation));
    quiesce_success = true;
    assert(audio.CloseLocalPlaybackOutput(initial.generation));
    assert(audio.PrepareLocalPlaybackOutput(initial.generation));
    assert(!audio.PrepareLocalPlaybackOutput(initial.generation));
    assert(!audio.WriteLocalPlaybackOutput(initial.generation, pcm));
    assert(audio.SetLocalPlaybackVolume(initial.generation, 10) == Volume::kRejected);
    assert(coordinator.Complete(initial));
    assert(audio.SetLocalPlaybackVolume(initial.generation, -1) == Volume::kRejected);
    assert(audio.SetLocalPlaybackVolume(initial.generation, 101) == Volume::kRejected);
    assert(audio.SetLocalPlaybackVolume(initial.generation + 1, 20) == Volume::kRejected);
    assert(codec.volume_attempts.empty() && !save_calls);
    assert(audio.SetLocalPlaybackVolume(initial.generation, 0) == Volume::kApplied);
    assert(saved_volume == 0 && codec.output_volume() == 0 && !codec.output_enabled());
    assert(audio.SetLocalPlaybackVolume(initial.generation, 100) == Volume::kApplied);
    assert(saved_volume == 100 && codec.output_volume() == 100);
    save_failures = 1;
    assert(audio.SetLocalPlaybackVolume(initial.generation, 25) == Volume::kFailed);
    assert(saved_volume == 100 && codec.output_volume() == 100 && save_calls == 4);
    codec.volume_failures = 1;
    assert(audio.SetLocalPlaybackVolume(initial.generation, 25) == Volume::kFailed);
    assert(codec.output_volume() == 100 && save_calls == 4);
    saved_volume = -1;
    const auto attempts = codec.volume_attempts.size();
    assert(audio.SetLocalPlaybackVolume(initial.generation, 25) == Volume::kFailed);
    assert(codec.volume_attempts.size() == attempts && save_calls == 4);
    saved_volume = 100;
    codec.volume_failures = 2;
    assert(audio.SetLocalPlaybackVolume(initial.generation, 25) == Volume::kUnsafe);
    save_failures = 2;
    assert(audio.SetLocalPlaybackVolume(initial.generation, 25) == Volume::kUnsafe);
    assert(audio.SetLocalPlaybackVolume(initial.generation, 70) == Volume::kApplied);
    std::puts("PASS: service volume transaction validates owner, persists mute and maximum, and restores hardware and storage after failure");
    assert(audio.WriteLocalPlaybackOutput(initial.generation, pcm));
    assert(!audio.WriteLocalPlaybackOutput(initial.generation, {}));
    assert(!audio.WriteLocalPlaybackOutput(initial.generation, std::vector<int16_t>(1921)));
    assert(audio.PauseLocalPlaybackOutput(initial.generation));
    assert(coordinator.SetLocalPlaybackPaused(initial.generation, true));
    assert(audio.SetLocalPlaybackVolume(initial.generation, 0) == Volume::kApplied);
    assert(saved_volume == 0 && !codec.output_enabled());
    assert(!audio.WriteLocalPlaybackOutput(initial.generation, pcm));
    assert(coordinator.SetLocalPlaybackPaused(initial.generation, false));
    assert(audio.WriteLocalPlaybackOutput(initial.generation, pcm));
    std::puts("PASS: actual service output requires prepared playback ownership and respects pause and PCM bounds");

    codec.block = true;
    std::thread writer([&] { assert(audio.WriteLocalPlaybackOutput(initial.generation, pcm)); });
    {
        std::unique_lock<std::mutex> lock(codec.mutex);
        assert(codec.changed.wait_for(lock, std::chrono::seconds(2), [&] { return codec.writing; }));
    }
    const auto recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
    assert(recording);
    const auto closes = codec.closes;
    const auto volume_attempts = codec.volume_attempts.size();
    assert(audio.SetLocalPlaybackVolume(initial.generation, 20) == Volume::kRejected);
    assert(codec.volume_attempts.size() == volume_attempts);
    assert(!audio.PauseLocalPlaybackOutput(initial.generation));
    assert(!audio.CloseLocalPlaybackOutput(initial.generation));
    assert(codec.closes == closes);
    assert(!audio.WriteLocalPlaybackOutput(initial.generation, pcm));
    assert(!audio.PrepareLocalPlaybackOutput(recording.generation));
    {
        std::lock_guard<std::mutex> lock(codec.mutex);
        codec.block = false; codec.changed.notify_all();
    }
    writer.join();
    codec.close_success = false;
    assert(!audio.CloseLocalPlaybackOutput(initial.generation));
    assert(!audio.CloseLocalPlaybackOutput(recording.generation));
    codec.close_success = true;
    assert(audio.CloseLocalPlaybackOutput(initial.generation));
    assert(coordinator.IsCurrent(recording.generation) && coordinator.Complete(recording));
    assert(!codec.output_enabled());
    std::puts("PASS: recording fences new playback writes and cleanup waits for in-flight output without releasing the new owner");

    assert(coordinator.Complete(coordinator.Begin(AudioSessionMode::kIdle)));
    const auto next = coordinator.Begin(AudioSessionMode::kLocalPlayback);
    assert(audio.PrepareLocalPlaybackOutput(next.generation) && coordinator.Complete(next));
    assert(!audio.CloseLocalPlaybackOutput(initial.generation));
    assert(!audio.WriteLocalPlaybackOutput(initial.generation, pcm));
    assert(audio.SetLocalPlaybackVolume(initial.generation, 20) == Volume::kRejected);
    codec.enable_success = false;
    const auto writes = codec.writes;
    assert(!audio.WriteLocalPlaybackOutput(next.generation, pcm) && codec.writes == writes);
    codec.enable_success = true; codec.write_success = false;
    assert(!audio.WriteLocalPlaybackOutput(next.generation, pcm));
    assert(audio.CloseLocalPlaybackOutput(next.generation));
    assert(coordinator.ReleaseLocalPlayback(next.generation));
    assert(quiesce_calls == 5);
    std::puts("PASS: stale cleanup cannot close a new playback owner and native failures propagate");
}
