#include "audio/recording_playback_task.h"
#include "audio/ogg_opus_writer.h"
#include "playback_codec_adapter.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include <opus.h>

namespace {
using namespace std::chrono_literals;
using State = RecordingPlaybackState;
std::filesystem::path root;
std::vector<RecordingOpusPacket> packets;

void Check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
template<class F> void Wait(F predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        Check(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(1ms);
    }
}
void Encode() {
    int error = 0;
    auto* encoder = opus_encoder_create(16000, 1, OPUS_APPLICATION_AUDIO, &error);
    Check(encoder && error == OPUS_OK, "reference encoder opens");
    for (unsigned frame = 0; frame < 50; ++frame) {
        std::vector<int16_t> pcm(320);
        for (size_t i = 0; i < pcm.size(); ++i)
            pcm[i] = static_cast<int16_t>(9000 * std::sin((frame * 320 + i) * 440.0 * 6.283185307179586 / 16000));
        RecordingOpusPacket packet;
        packet.bytes.resize(4096);
        const int bytes = opus_encode(encoder, pcm.data(), 320, packet.bytes.data(), packet.bytes.size());
        Check(bytes > 0, "reference packet encodes");
        packet.bytes.resize(bytes);
        packets.push_back(std::move(packet));
    }
    opus_encoder_destroy(encoder);
}

std::string MakeFile() {
    const auto path = (root / "recording.opus").string();
    OggOpusWriter writer;
    Check(writer.Open(path.c_str(), 16000, 1, 320), "fixture recording opens");
    for (const auto& packet : packets) Check(writer.WritePacket(packet.bytes.data(), packet.bytes.size()), "fixture recording writes");
    Check(writer.Close(), "fixture recording closes");
    return path;
}
RecordingOpusInfo Info() {
    RecordingOpusInfo info;
    info.valid = true;
    info.channels = 1;
    info.input_sample_rate = 16000;
    return info;
}

void TestDecodeAndTrimming() {
    RecordingPlaybackDecoder decoder;
    auto info = Info();
    info.pre_skip_48k = 4000;
    Check(decoder.Open(info, 16000), "native decoder wrapper opens");
    size_t output = 0;
    std::vector<int16_t> pcm;
    for (size_t i = 0; i < 5; ++i) {
        auto packet = packets[i];
        if (i == 4) { packet.eos = true; packet.granule_position = 4650; }
        Check(decoder.Decode(packet, pcm), "actual PCM decoded");
        if (i < 4) Check(pcm.empty(), "pre-skip can cover multiple packets");
        output += pcm.size();
    }
    Check(output == 216 && decoder.DecodedSamples() == 1600, "pre-skip and final trim use 48 kHz clock");
    Check(!decoder.Decode(packets[5], pcm) && pcm.empty(), "no packet after EOS");
    Check(decoder.Close(), "decoder close");

    info = Info(); info.legacy_audream_granules = true;
    Check(decoder.Open(info, 16000), "legacy decoder opens");
    auto packet = packets[0]; packet.eos = true; packet.granule_position = 320;
    Check(decoder.Decode(packet, pcm) && pcm.size() == 320, "legacy granule does not trim two thirds of audio");
    Check(decoder.Close(), "legacy close");
    info = Info();
    Check(!decoder.Open(info, 44100), "unsupported output rate rejected");
    for (const auto rate : {8000, 12000, 16000, 24000, 48000}) {
        Check(decoder.Open(info, rate), "supported native rate");
        Check(decoder.Decode(packets[0], pcm) && pcm.size() == static_cast<size_t>(rate / 50), "direct output-rate decoding");
        Check(decoder.Close(), "rate decoder closes");
    }
}

void TestDecodeFailuresAndGain() {
    RecordingPlaybackDecoder decoder;
    auto info = Info();
    std::vector<int16_t> pcm;
    for (const auto fault : {HostPlaybackCodec::Fault::kDecode, HostPlaybackCodec::Fault::kConsumption,
         HostPlaybackCodec::Fault::kFormat, HostPlaybackCodec::Fault::kSize}) {
        Check(decoder.Open(info, 16000), "fault decoder opens");
        HostPlaybackCodec::fault = fault;
        Check(!decoder.Decode(packets[0], pcm) && pcm.empty(), "invalid output is never published");
        HostPlaybackCodec::fault = HostPlaybackCodec::Fault::kNone;
        Check(!decoder.Decode(packets[1], pcm), "failed decoder cannot continue damaged state");
        Check(decoder.Close(), "fault decoder closes");
    }
    HostPlaybackCodec::fault = HostPlaybackCodec::Fault::kOpen;
    Check(!decoder.Open(info, 16000), "open failure propagates");
    HostPlaybackCodec::fault = HostPlaybackCodec::Fault::kNone;
    Check(decoder.Open(info, 16000), "gain reference opens");
    std::vector<int16_t> reference;
    Check(decoder.Decode(packets[0], reference), "gain reference decodes");
    info.output_gain_q8 = -6 * 256;
    Check(decoder.Open(info, 16000) && decoder.Decode(packets[0], pcm), "header attenuation decodes");
    for (size_t i = 0; i < pcm.size(); ++i)
        Check(pcm[i] == std::lround(reference[i] * std::pow(10.0, -6.0 / 20)), "only declared output gain applied");
    HostPlaybackCodec::fail_close = true;
    Check(!decoder.Close() && HostPlaybackCodec::handles == 1, "failed close retains decoder");
    HostPlaybackCodec::fail_close = false;
    Check(decoder.Close() && HostPlaybackCodec::handles == 0, "explicit close recovery");
}

struct Output {
    AudioSessionCoordinator& coordinator;
    std::mutex mutex;
    std::condition_variable changed;
    bool block_write = false;
    bool block_prepare = false;
    bool allow_close = true;
    bool allow_acquire = true;
    bool allow_write = true;
    bool allow_prepare = true;
    bool allow_pause = true;
    bool in_write = false;
    bool in_prepare = false;
    bool block_volume = false;
    bool in_volume = false;
    bool block_pause = false;
    bool in_pause = false;
    std::atomic<int> volume_calls{0};
    std::atomic<int> volume{70};
    std::atomic<PlaybackVolumeStatus> volume_result{PlaybackVolumeStatus::kApplied};
    std::atomic<int> leases{0};
    std::atomic<int> writes{0};
    std::atomic<int> closes{0};
    std::atomic<int> pauses{0};
    std::vector<int16_t> audio;
    explicit Output(AudioSessionCoordinator& value) : coordinator(value) {}

    RecordingPlaybackTask::Hooks Hooks() {
        RecordingPlaybackTask::Hooks hooks;
        hooks.acquire_storage = [this] { if (!allow_acquire) return false; ++leases; return true; };
        hooks.release_storage = [this] { Check(leases-- == 1, "one storage release per acquire"); };
        hooks.prepare_output = [this](uint64_t generation) {
            std::unique_lock<std::mutex> lock(mutex);
            in_prepare = true; changed.notify_all();
            changed.wait(lock, [this] { return !block_prepare; });
            in_prepare = false;
            return allow_prepare && coordinator.IsCurrent(generation);
        };
        hooks.write_output = [this](uint64_t generation, const std::vector<int16_t>& pcm) {
            std::unique_lock<std::mutex> lock(mutex);
            if (!coordinator.OwnsLocalPlaybackResources(generation) || !allow_write) return false;
            in_write = true; changed.notify_all();
            changed.wait(lock, [this] { return !block_write; });
            audio.insert(audio.end(), pcm.begin(), pcm.end());
            ++writes; in_write = false;
            return true;
        };
        hooks.pause_output = [this](uint64_t) {
            std::unique_lock<std::mutex> lock(mutex);
            ++pauses; in_pause = true; changed.notify_all();
            changed.wait(lock, [this] { return !block_pause; });
            in_pause = false; return allow_pause;
        };
        hooks.set_volume = [this](uint64_t generation, int value) {
            std::unique_lock<std::mutex> lock(mutex);
            Check(!in_write && !in_prepare, "volume cannot overlap output or preparation");
            Check(coordinator.OwnsLocalPlaybackResources(generation), "volume starts with exact playback owner");
            ++volume_calls; in_volume = true; changed.notify_all();
            changed.wait(lock, [this] { return !block_volume; });
            in_volume = false;
            if (volume_result == PlaybackVolumeStatus::kApplied) volume = value;
            return volume_result.load();
        };
        hooks.close_output = [this](uint64_t) {
            std::lock_guard<std::mutex> lock(mutex);
            Check(!in_write && !in_prepare && !in_volume && !in_pause, "no close across live output operations");
            ++closes;
            return allow_close;
        };
        return hooks;
    }
    void AwaitWrite() {
        std::unique_lock<std::mutex> lock(mutex);
        Check(changed.wait_for(lock, 3s, [this] { return in_write; }), "output entered");
    }
    void UnblockWrite() { std::lock_guard<std::mutex> lock(mutex); block_write = false; changed.notify_all(); }
};

void TestFileToOutputAndRepeat() {
    const auto path = MakeFile();
    AudioSessionCoordinator coordinator;
    Output output(coordinator);
    RecordingPlaybackTask task(coordinator, output.Hooks(), 16000);
    auto generation = task.Start("recording", path, std::filesystem::file_size(path));
    Check(generation != 0, "playback request accepted");
    Wait([&] {
        const auto debug = task.Snapshot();
        if (debug.state == State::kFailed) {
            std::cerr << "Playback failure: error=" << int(debug.error) << " read=" << int(debug.read_error)
                << " samples=" << debug.output_samples << " total=" << debug.total_samples << '\n';
            Check(false, "recording failed before end");
        }
        return debug.state == State::kEnded;
    }, "recording reaches end after real decode");
    const auto snapshot = task.Snapshot();
    Check(snapshot.output_samples == 16000 && snapshot.end == RecordingEndKind::kLegacyPageBoundary &&
          !snapshot.resources_held && output.leases == 0 && !coordinator.BlocksSleep(), "full legacy second delivered then released");
    Check(output.audio.size() == 16000 && output.writes == 50 && output.closes == 1, "actual packets became output PCM");
    double energy = 0;
    for (const auto sample : output.audio) energy += double(sample) * sample;
    Check(std::sqrt(energy / output.audio.size()) > 5000, "decoded PCM is not silence");
    const auto second = task.Start("recording", path, std::filesystem::file_size(path));
    Check(second != 0 && second != generation && !task.Stop(generation, 10), "stale stop cannot cancel new playback");
    Wait([&] { return task.Snapshot().state == State::kEnded; }, "repeat reaches end");
    Check(task.Snapshot().output_samples == 16000 && output.leases == 0, "repeat resets position and releases");
}

void TestPauseAndRecordingHandoff() {
    const auto path = MakeFile();
    AudioSessionCoordinator coordinator;
    Output output(coordinator); output.block_write = true;
    RecordingPlaybackTask task(coordinator, output.Hooks(), 16000);
    const auto generation = task.Start("recording", path, std::filesystem::file_size(path));
    output.AwaitWrite();
    Check(task.SetPaused(generation, true), "pause requested during output");
    Check(task.Snapshot().state == State::kPausing && output.pauses == 0 && output.leases == 1, "pause waits for real write");
    output.UnblockWrite();
    Wait([&] { return task.Snapshot().state == State::kPaused; }, "pause acknowledged");
    const auto paused_samples = task.Snapshot().output_samples;
    std::this_thread::sleep_for(30ms);
    Check(paused_samples == 320 && task.Snapshot().output_samples == paused_samples && coordinator.BlocksSleep(),
          "paused playback retains file and exact output position");
    {
        std::lock_guard<std::mutex> lock(output.mutex); output.block_write = true;
    }
    Check(task.SetPaused(generation, false), "resume queued");
    output.AwaitWrite();
    const auto recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
    Check(static_cast<bool>(recording), "recording takes new owner");
    Check(!task.StopForRecording(recording.generation, 5), "in-flight output prevents premature recording handoff");
    Check(output.leases == 1 && output.closes == 0 && coordinator.IsCurrent(recording.generation), "handoff timeout retains resources");
    output.UnblockWrite();
    Check(task.StopForRecording(recording.generation, 2000), "handoff confirms actual resource release");
    Check(output.writes == 2 && output.closes == 1 && output.leases == 0, "no later audio after recording preemption");
    Check(coordinator.Complete(recording) && !task.SetPaused(generation, false), "old playback cannot resume recording owner");
}

void TestNonblockingStop() {
    const auto path = MakeFile();
    AudioSessionCoordinator coordinator;
    Output output(coordinator); output.block_write = true;
    RecordingPlaybackTask task(coordinator, output.Hooks(), 16000);
    const auto generation = task.Start("recording", path, std::filesystem::file_size(path));
    output.AwaitWrite();
    Check(!task.RequestStop(generation + 1), "stale nonblocking stop is rejected");
    Check(task.RequestStop(generation), "UI stop does not wait for blocked native write");
    Check(task.Snapshot().state == State::kStopping && task.Snapshot().resources_held && output.leases == 1,
        "accepted stop retains resources and reports stopping");
    output.UnblockWrite();
    Check(task.Stop(generation, 2000) && task.Snapshot().state == State::kStopped, "write and cleanup complete stop");
    Check(!task.RequestStop(generation), "closed output does not accept another stop");
}

void TestVolumeTransactions() {
    using Volume = PlaybackVolumeStatus;
    const auto path = MakeFile();
    AudioSessionCoordinator coordinator;
    Output output(coordinator); output.block_write = true;
    RecordingPlaybackTask task(coordinator, output.Hooks(), 16000);
    const auto generation = task.Start("recording", path, std::filesystem::file_size(path));
    output.AwaitWrite();
    Check(task.Snapshot().volume_supported, "output advertises volume capability");
    Check(!task.SetVolume(generation + 1, 1, 0) && !task.SetVolume(generation, 0, 0) &&
        !task.SetVolume(generation, 1, -1) && !task.SetVolume(generation, 1, 101), "invalid volume requests rejected");
    Check(task.SetVolume(generation, 1, 0), "mute request accepted without waiting for native write");
    Check(task.Snapshot().volume.status == Volume::kPending && output.volume_calls == 0 && output.volume == 70,
        "accepted volume is not applied until output yields");
    Check(!task.SetVolume(generation, 2, 100), "in-flight volume cannot be overwritten");
    Check(task.SetPaused(generation, true), "pause can follow queued volume");
    output.UnblockWrite();
    Wait([&] { return task.Snapshot().state == State::kPaused; }, "volume then pause completed");
    Check(task.Snapshot().volume.status == Volume::kApplied && output.volume == 0 && output.volume_calls == 1,
        "actual worker applies mute before acknowledging");
    Check(!task.SetVolume(generation, 1, 50), "completed request cannot be replayed");
    const auto writes = output.writes.load();
    Check(task.SetVolume(generation, 2, 100), "paused volume accepted");
    Wait([&] { return task.Snapshot().volume.status == Volume::kApplied; }, "paused volume completed");
    Check(output.volume == 100 && output.writes == writes && task.Snapshot().state == State::kPaused,
        "changing paused volume never resumes PCM output");
    output.volume_result = Volume::kFailed;
    Check(task.SetVolume(generation, 3, 25), "retryable failure queued");
    Wait([&] { return task.Snapshot().volume.status == Volume::kFailed; }, "save failure exposed");
    Check(output.volume == 100 && task.Snapshot().state == State::kPaused && output.leases == 1,
        "restored volume failure does not corrupt or release playback");
    output.volume_result = Volume::kUnsafe;
    Check(task.SetVolume(generation, 4, 25), "unsafe output failure queued");
    Wait([&] { return task.Snapshot().state == State::kFailed; }, "unrestored volume closes output");
    Check(task.Snapshot().error == RecordingPlaybackError::kOutput && task.Snapshot().volume.status == Volume::kUnsafe &&
        !task.Snapshot().resources_held && output.leases == 0, "unsafe volume never reports success or keeps playing");
}

void TestVolumeStopAndRecordingHandoff() {
    using Volume = PlaybackVolumeStatus;
    const auto path = MakeFile();
    AudioSessionCoordinator coordinator;
    Output output(coordinator); output.block_write = true;
    RecordingPlaybackTask task(coordinator, output.Hooks(), 16000);
    auto generation = task.Start("recording", path, std::filesystem::file_size(path));
    output.AwaitWrite();
    Check(task.SetVolume(generation, 1, 0) && task.RequestStop(generation), "stop supersedes queued mute");
    output.UnblockWrite();
    Check(task.Stop(generation, 2000), "stop completes after write");
    Check(task.Snapshot().volume.status == Volume::kCancelled && !output.volume_calls && output.volume == 70,
        "queued volume cancellation never touches hardware");
    {
        std::lock_guard<std::mutex> lock(output.mutex); output.block_write = true; output.block_volume = true;
    }
    generation = task.Start("recording", path, std::filesystem::file_size(path));
    output.AwaitWrite();
    Check(task.SetVolume(generation, 1, 0), "new generation resets volume requests");
    output.UnblockWrite();
    {
        std::unique_lock<std::mutex> lock(output.mutex);
        Check(output.changed.wait_for(lock, 3s, [&] { return output.in_volume; }), "native volume entered");
    }
    const auto recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
    Check(static_cast<bool>(recording), "recording preempts native volume owner");
    Check(!task.StopForRecording(recording.generation, 5), "recording waits for live volume transaction");
    Check(output.leases == 1 && task.Snapshot().resources_held && task.Snapshot().volume.status == Volume::kPending,
        "live volume retains resources even after stop timeout");
    Check(!task.SetVolume(generation, 2, 20), "preempted playback cannot queue another volume");
    {
        std::lock_guard<std::mutex> lock(output.mutex); output.block_volume = false; output.changed.notify_all();
    }
    Check(task.StopForRecording(recording.generation, 2000) && coordinator.Complete(recording), "recording proceeds after real cleanup");
    Check(task.Snapshot().volume.status == Volume::kApplied && output.volume == 0 && output.leases == 0,
        "already executing transaction reports its actual outcome before handoff");
}

void TestIndexedDurationAndSeek() {
    const auto path = (root / "long.opus").string();
    OggOpusWriter writer;
    // Deliberately wrong legacy granule increment; TOC timing must win.
    Check(writer.Open(path.c_str(), 16000, 1, 160), "long index fixture opens");
    for (size_t i = 0; i < 30000; ++i) {
        const auto& packet = packets[i % packets.size()];
        Check(writer.WritePacket(packet.bytes.data(), packet.bytes.size()), "long index fixture writes");
    }
    Check(writer.Close(), "long index fixture closes");
    RecordingPlaybackSource source;
    const auto decodes = HostPlaybackCodec::decodes.load();
    unsigned yields = 0;
    Check(source.Open(path.c_str(), std::filesystem::file_size(path), 16000, {}, [&] { ++yields; }), "index scans real packets");
    Check(source.TotalSamples() == 30000ULL * 320 && source.IndexSize() <= 128 && yields >= 30000 / 64,
        "legacy duration is exact with bounded sparse index and work yields");
    Check(HostPlaybackCodec::decodes == decodes, "duration scan does not decode audio");
    uint64_t raw = 0;
    const auto inspections = source.InspectedPackets();
    const auto target = source.TotalSamples() - 177;
    Check(source.Seek(target, raw), "late target located from sparse index");
    Check(raw <= target - 1280 && target - 1280 - raw < 320 && source.InspectedPackets() - inspections < 512,
        "late seek has 80 ms preroll without a whole-file rescan");
    Check(HostPlaybackCodec::decodes == decodes, "packet location does not decode discarded prefix");
    Check(source.Seek(100, raw) && raw == 0, "near-start seek starts at original beginning");
    Check(!source.Seek(source.TotalSamples() + 1, raw), "out of range seek rejected");
    Check(!source.Seek(100, raw, [] { return true; }), "seek cancellation does not start I/O");
    Check(source.Close(), "indexed source closes");
    Check(!source.Seek(100, raw), "closed source cannot reuse its index");
    bool cancelled = false;
    Check(!source.Open(path.c_str(), std::filesystem::file_size(path), 16000, [&] { return cancelled; }, [&] { cancelled = true; }),
        "index scan is cancellable at bounded work yields");
    Check(source.Close(), "cancelled index closes");
}

void TestPlaybackSeeking() {
    using Seek = PlaybackSeekStatus;
    const auto path = MakeFile();
    AudioSessionCoordinator coordinator;
    Output output(coordinator); output.block_write = true;
    RecordingPlaybackTask task(coordinator, output.Hooks(), 16000);
    const auto generation = task.Start("recording", path, std::filesystem::file_size(path));
    output.AwaitWrite();
    Check(task.Snapshot().duration_known && task.Snapshot().total_samples == 16000, "worker publishes verified duration");
    Check(task.SetPaused(generation, true), "pause before seek");
    output.UnblockWrite();
    Wait([&] { return task.Snapshot().state == State::kPaused; }, "pause reached");
    const auto writes = output.writes.load();
    const auto decodes = HostPlaybackCodec::decodes.load();
    Check(!task.Seek(generation + 1, 1, 100) && !task.Seek(generation, 0, 100) && !task.Seek(generation, 1, 16001),
        "stale, zero and out-of-range seek requests rejected");
    Check(task.Seek(generation, 1, 10003), "sample-accurate paused seek accepted");
    Wait([&] { return task.Snapshot().seek.status == Seek::kApplied; }, "preroll reaches target");
    Check(task.Snapshot().state == State::kPaused && task.Snapshot().output_samples == 10003 && output.writes == writes,
        "preroll and paused seek do not reach speaker or resume playback");
    Check(HostPlaybackCodec::decodes - decodes >= 4 && HostPlaybackCodec::decodes - decodes <= 6,
        "late target decodes only bounded preroll, not the entire prefix");
    Check(!task.Seek(generation, 1, 100), "seek receipt cannot be replayed");
    Check(task.Seek(generation, 2, 77), "backward seek accepted");
    Wait([&] { return task.Snapshot().seek.status == Seek::kApplied; }, "backward target reached");
    Check(task.Snapshot().output_samples == 77 && output.writes == writes, "backward seek does not add discarded samples to output");
    Check(task.SetPaused(generation, false), "resume at requested sample");
    Wait([&] { return task.Snapshot().state == State::kEnded; }, "seeked recording completes");
    Check(task.Snapshot().output_samples == 16000 && output.audio.size() == 320 + 16000 - 77,
        "only suffix after target is output and final position remains duration");
    RecordingPlaybackDecoder reference;
    auto info = Info(); info.legacy_audream_granules = true;
    Check(reference.Open(info, 16000), "seek reference decoder opens");
    std::vector<int16_t> linear, pcm;
    for (const auto& packet : packets) {
        Check(reference.Decode(packet, pcm), "seek reference decodes");
        linear.insert(linear.end(), pcm.begin(), pcm.end());
    }
    Check(std::equal(output.audio.begin() + 320, output.audio.end(), linear.begin() + 77), "near-start seek PCM exactly matches linear decode");
    Check(reference.Close(), "seek reference closes");
}

void TestSeekFailureAndHandoff() {
    using Seek = PlaybackSeekStatus;
    const auto path = MakeFile();
    AudioSessionCoordinator coordinator;
    Output output(coordinator); output.block_write = true;
    RecordingPlaybackTask task(coordinator, output.Hooks(), 16000);
    auto generation = task.Start("recording", path, std::filesystem::file_size(path));
    output.AwaitWrite();
    Check(task.Seek(generation, 1, 8000) && task.RequestStop(generation), "stop supersedes queued seek");
    output.UnblockWrite();
    Check(task.Stop(generation, 2000) && task.Snapshot().seek.status == Seek::kCancelled && output.pauses == 0,
        "cancelled queued seek does not drain or reset decoder");
    {
        std::lock_guard<std::mutex> lock(output.mutex); output.block_write = true; output.block_pause = true;
    }
    generation = task.Start("recording", path, std::filesystem::file_size(path));
    output.AwaitWrite();
    Check(task.Seek(generation, 1, 8000), "new seek accepted");
    output.UnblockWrite();
    {
        std::unique_lock<std::mutex> lock(output.mutex);
        Check(output.changed.wait_for(lock, 3s, [&] { return output.in_pause; }), "seek output drain entered");
    }
    Check(task.Snapshot().state == State::kSeeking && task.Snapshot().output_samples == 320,
        "target is not published before native drain completes");
    const auto recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
    Check(recording && !task.StopForRecording(recording.generation, 5) && output.leases == 1,
        "recording cannot bypass a blocked seek drain");
    {
        std::lock_guard<std::mutex> lock(output.mutex); output.block_pause = false; output.changed.notify_all();
    }
    Check(task.StopForRecording(recording.generation, 2000) && task.Snapshot().seek.status == Seek::kCancelled,
        "recording gets resources only after seek cancellation and actual cleanup");
    Check(coordinator.Complete(recording) && coordinator.Complete(coordinator.Begin(AudioSessionMode::kIdle)), "test recording releases");
    {
        std::lock_guard<std::mutex> lock(output.mutex); output.block_write = true;
    }
    generation = task.Start("recording", path, std::filesystem::file_size(path));
    output.AwaitWrite();
    HostPlaybackCodec::fault = HostPlaybackCodec::Fault::kReset;
    Check(task.Seek(generation, 1, 8000), "reset failure seek accepted");
    output.UnblockWrite();
    Wait([&] { return task.Snapshot().state == State::kFailed; }, "reset error retires playback");
    HostPlaybackCodec::fault = HostPlaybackCodec::Fault::kNone;
    Check(task.Snapshot().error == RecordingPlaybackError::kDecode && task.Snapshot().seek.status == Seek::kFailed && !output.leases,
        "reset failure is not a successful jump");
}

void TestPreparationAndCleanupHandoff() {
    const auto path = MakeFile();
    AudioSessionCoordinator coordinator;
    Output output(coordinator); output.block_prepare = true;
    RecordingPlaybackTask task(coordinator, output.Hooks(), 16000);
    const auto generation = task.Start("recording", path, std::filesystem::file_size(path));
    {
        std::unique_lock<std::mutex> lock(output.mutex);
        Check(output.changed.wait_for(lock, 3s, [&] { return output.in_prepare; }), "preparation entered");
    }
    const auto recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
    Check(recording && !task.StopForRecording(recording.generation, 5), "preparing playback is drained before recording");
    {
        std::lock_guard<std::mutex> lock(output.mutex);
        output.allow_close = false; output.block_prepare = false; output.changed.notify_all();
    }
    Wait([&] { return task.Snapshot().state == State::kCleanupFailed; }, "cleanup failure explicit");
    Check(output.leases == 1 && task.Snapshot().resources_held && coordinator.IsCurrent(recording.generation), "failed old close retains storage without clearing new owner");
    Check(!task.Start("other", path, std::filesystem::file_size(path)), "cleanup failure prohibits replacement reader");
    {
        std::lock_guard<std::mutex> lock(output.mutex); output.allow_close = true;
    }
    Check(task.StopForRecording(recording.generation, 2000), "explicit cleanup retry succeeds");
    Check(output.writes == 0 && output.leases == 0 && !coordinator.ReleaseLocalPlayback(generation), "preparation never publishes stale PCM");
    Check(coordinator.Complete(recording), "new recording owner survives recovery");
}

void TestFailuresDoNotReportEnd() {
    const auto path = MakeFile();
    for (int fault = 0; fault < 5; ++fault) {
        AudioSessionCoordinator coordinator;
        Output output(coordinator);
        output.allow_acquire = fault != 0;
        output.allow_prepare = fault != 1;
        output.allow_write = fault != 2;
        RecordingPlaybackTask task(coordinator, output.Hooks(), 16000);
        if (fault == 3) HostPlaybackCodec::fault = HostPlaybackCodec::Fault::kDecode;
        const auto generation = task.Start("recording", path, std::filesystem::file_size(path) + (fault == 4));
        Check(generation != 0, "failed playback queued");
        Wait([&] { return task.Snapshot().state == State::kFailed; }, "playback failure completes cleanup");
        Check(task.Snapshot().output_samples == 0 && output.leases == 0 && !coordinator.BlocksSleep(), "failed playback is not completed or leaked");
        HostPlaybackCodec::fault = HostPlaybackCodec::Fault::kNone;
    }
}

void TestDecoderCleanupFailure() {
    const auto path = MakeFile();
    AudioSessionCoordinator coordinator;
    Output output(coordinator); output.block_write = true;
    RecordingPlaybackTask task(coordinator, output.Hooks(), 16000);
    const auto generation = task.Start("recording", path, std::filesystem::file_size(path));
    output.AwaitWrite();
    HostPlaybackCodec::fail_close = true;
    Check(!task.Stop(generation, 5), "stop waits in-flight native write");
    output.UnblockWrite();
    Wait([&] { return task.Snapshot().state == State::kCleanupFailed; }, "decoder close failure retained");
    Check(coordinator.Mode() == AudioSessionMode::kFault && output.leases == 1 && HostPlaybackCodec::handles == 1,
          "failed decoder closure retains all leases");
    HostPlaybackCodec::fail_close = false;
    Check(task.Stop(generation, 2000) && output.leases == 0 && HostPlaybackCodec::handles == 0, "decoder cleanup retry releases");
    Check(task.Snapshot().state == State::kFailed && !coordinator.BlocksSleep(), "failure receipt retained after safe recovery");
}
void TestIndependentStandardFile() {
    const char* path = std::getenv("AUDREAM_OPUS_FIXTURE");
    if (!path || !*path) return;
    AudioSessionCoordinator coordinator;
    Output output(coordinator);
    RecordingPlaybackTask task(coordinator, output.Hooks(), 16000);
    Check(task.Start("standard", path, std::filesystem::file_size(path)) != 0, "independent standard file accepted");
    Wait([&] { return !task.Snapshot().resources_held; }, "independent standard file retired");
    const auto result = task.Snapshot();
    Check(result.state == State::kEnded && result.end == RecordingEndKind::kEos &&
        result.output_samples == 32000 && output.audio.size() == 32000,
        "FFmpeg two-second file trims pre-skip and final padding to exactly two seconds");
    const auto linear = output.audio;
    for (const uint64_t target : {0ULL, 77ULL, 27313ULL, 31999ULL, 32000ULL}) {
        {
            std::lock_guard<std::mutex> lock(output.mutex);
            output.audio.clear(); output.block_write = true;
        }
        const auto generation = task.Start("standard", path, std::filesystem::file_size(path));
        output.AwaitWrite();
        Check(task.Snapshot().total_samples == 32000 && task.Snapshot().duration_known, "standard duration excludes pre-skip and padding");
        Check(task.SetPaused(generation, true), "standard seek starts paused");
        output.UnblockWrite();
        Wait([&] { return task.Snapshot().state == State::kPaused; }, "standard output paused");
        const auto prefix = output.audio.size();
        Check(task.Seek(generation, 1, target), "standard sample target accepted");
        Wait([&] { return task.Snapshot().seek.status == PlaybackSeekStatus::kApplied; }, "standard sample target reached");
        Check(task.Snapshot().output_samples == target && task.Snapshot().state == State::kPaused && output.audio.size() == prefix,
            "standard seek trims preroll silently and preserves pause including at exact end");
        Check(task.SetPaused(generation, false), "standard seek resumes");
        Wait([&] { return !task.Snapshot().resources_held; }, "standard seek retires");
        Check(task.Snapshot().state == State::kEnded && task.Snapshot().output_samples == 32000 &&
            output.audio.size() == prefix + 32000 - target, "standard seek outputs exactly the audible suffix");
        if (target < 1280) Check(std::equal(output.audio.begin() + prefix, output.audio.end(), linear.begin() + target),
            "standard near-start seek preserves full pre-skip and matches linear PCM");
    }
    std::cout << "PASS: independent FFmpeg standard Opus file decodes to exactly 32000 output samples\n";
    std::cout << "PASS: standard Opus seeks preserve pre-skip, final trim, pause and exact end\n";
}
}  // namespace

int main(int argc, char** argv) {
    Check(argc == 2, "test directory supplied"); root = argv[1];
    Encode();
    TestDecodeAndTrimming();
    TestDecodeFailuresAndGain();
    TestFileToOutputAndRepeat();
    TestPauseAndRecordingHandoff();
    TestNonblockingStop();
    TestVolumeTransactions();
    TestVolumeStopAndRecordingHandoff();
    TestIndexedDurationAndSeek();
    TestPlaybackSeeking();
    TestSeekFailureAndHandoff();
    TestPreparationAndCleanupHandoff();
    TestFailuresDoNotReportEnd();
    TestDecoderCleanupFailure();
    TestIndependentStandardFile();
    Check(HostPlaybackCodec::handles == 0, "all decoder handles released");
    std::cout << "13 recording playback test groups passed\n";
}
