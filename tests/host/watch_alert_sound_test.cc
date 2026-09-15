#include "watch/watch_alert_sound.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {
template<class Predicate> void Wait(Predicate predicate) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < limit);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
struct Output {
    std::mutex mutex;
    std::condition_variable changed;
    bool blocked = false, entered = false;
    bool prepare_ok = true, write_ok = true;
    std::atomic<bool> close_ok{true};
    std::atomic<unsigned> prepares{0}, closes{0}, writes{0}, releases{0};
    std::vector<int16_t> samples;
    WatchAlertSound::Hooks Hooks(AudioSessionCoordinator& coordinator) {
        return {
            [&](uint64_t generation) {
                ++prepares;
                assert(coordinator.OwnsLocalPlaybackPreparation(generation));
                return prepare_ok;
            },
            [&](uint64_t generation, const std::vector<int16_t>& pcm) {
                assert(coordinator.OwnsLocalPlaybackResources(generation));
                ++writes;
                std::unique_lock<std::mutex> lock(mutex);
                entered = true; changed.notify_all();
                changed.wait(lock, [&] { return !blocked; });
                samples.insert(samples.end(), pcm.begin(), pcm.end());
                return write_ok;
            },
            [&](uint64_t) { ++closes; return close_ok.load(); },
            [&] { ++releases; },
        };
    }
    void Unblock() {
        std::lock_guard<std::mutex> lock(mutex); blocked = false; changed.notify_all();
    }
    void WaitEntered() {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }));
    }
};
void TestToneAndRepeatWindow() {
    for (uint32_t rate : {16000U, 48000U}) {
        AudioSessionCoordinator coordinator;
        Output output;
        WatchAlertSound sound(coordinator, output.Hooks(coordinator), rate);
        sound.Poll(0, 1000);
        assert(output.prepares == 0);
        sound.Poll(1, 1000);
        Wait([&] { return output.releases == 1; });
        assert(!sound.Snapshot().failed && !coordinator.BlocksSleep());
        assert(output.writes == 25 && output.closes == 1 && output.samples.size() == rate / 2);
        assert(output.samples.front() == 0 && output.samples.back() == 0);
        bool audible = false;
        for (size_t i = 0; i < output.samples.size(); ++i) {
            const auto sample = output.samples[i];
            assert(sample >= -8192 && sample <= 8192);
            if (i >= rate / 5 && i < rate * 3 / 10) assert(sample == 0);
            audible |= sample > 1000;
        }
        assert(audible);
        sound.Poll(1, 5999); assert(output.prepares == 1);
        sound.Poll(1, 6000); Wait([&] { return output.releases == 2; });
        sound.Poll(1, 61000); assert(output.prepares == 2);
        sound.Poll(1, 66000); assert(output.prepares == 2);
        sound.Poll(1, 66000, true); Wait([&] { return output.releases == 3; });
        sound.Silence(); sound.Poll(1, 72000); assert(output.prepares == 3);
        sound.Poll(0, 73000); sound.Poll(0x20, 74000); Wait([&] { return output.releases == 4; });
    }
    std::puts("PASS: alert PCM has two ramped tones, bounded repetition, dismissal and new-event rearming");
}
void TestBusySessionsAndExpiredAlerts() {
    for (auto mode : {AudioSessionMode::kLocalRecording, AudioSessionMode::kFileTransfer,
        AudioSessionMode::kAssistantConnecting, AudioSessionMode::kMaintenance,
        AudioSessionMode::kSleeping, AudioSessionMode::kLocalPlayback}) {
        AudioSessionCoordinator coordinator;
        const auto existing = coordinator.Begin(mode);
        assert(existing && coordinator.Complete(existing));
        Output output;
        WatchAlertSound sound(coordinator, output.Hooks(coordinator), 16000);
        sound.Poll(1, 1000); sound.Poll(1, 62000);
        assert(output.prepares == 0 && coordinator.IsCurrent(existing.generation) && coordinator.Mode() == mode);
    }
    AudioSessionCoordinator coordinator;
    const auto assistant = coordinator.Begin(AudioSessionMode::kAssistantConnecting);
    assert(coordinator.Complete(assistant));
    Output output;
    WatchAlertSound sound(coordinator, output.Hooks(coordinator), 16000);
    sound.Poll(1, 1000);
    assert(coordinator.EndAssistant(assistant.generation));
    sound.Poll(1, 61000); assert(output.prepares == 0);
    sound.Poll(1, 62000, true); Wait([&] { return output.releases == 1; });
    std::puts("PASS: alerts do not steal busy audio and expired deferred alerts do not ring later");
}
void TestRecordingWaitsForBlockedOutput() {
    AudioSessionCoordinator coordinator;
    Output output; output.blocked = true;
    WatchAlertSound sound(coordinator, output.Hooks(coordinator), 16000);
    sound.Poll(1, 1000); output.WaitEntered();
    const auto alert_generation = sound.Snapshot().generation;
    const auto recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
    assert(recording && recording.interrupted_purpose == AudioSessionMode::kLocalPlayback);
    assert(!sound.StopForRecording(recording.generation, 10));
    assert(sound.Snapshot().active && output.closes == 0 && coordinator.HoldsExclusiveResources(recording.generation));
    output.Unblock();
    assert(sound.StopForRecording(recording.generation, 2000));
    Wait([&] { return output.releases == 1; });
    assert(output.writes == 1 && output.closes == 1 && coordinator.IsCurrent(recording.generation));
    assert(!coordinator.ReleaseLocalPlayback(alert_generation));
    assert(coordinator.Complete(recording));
    const auto idle = coordinator.Begin(AudioSessionMode::kIdle); assert(coordinator.Complete(idle));
    sound.Poll(1, 7000); assert(output.prepares == 1);
    std::puts("PASS: recording preemption waits for real output closure and suppresses obsolete chimes");
}
void TestFailuresAndCleanupRetry() {
    for (int phase = 0; phase < 2; ++phase) {
        AudioSessionCoordinator coordinator;
        Output output;
        output.prepare_ok = phase != 0; output.write_ok = phase != 1;
        WatchAlertSound sound(coordinator, output.Hooks(coordinator), 16000);
        sound.Poll(1, 1000); Wait([&] { return output.releases == 1; });
        assert(sound.Snapshot().failed && !sound.Snapshot().active && !coordinator.BlocksSleep() && output.closes == 1);
        sound.Poll(1, 7000); assert(output.prepares == 1);
    }
    AudioSessionCoordinator coordinator;
    Output output; output.close_ok = false;
    WatchAlertSound sound(coordinator, output.Hooks(coordinator), 16000);
    sound.Poll(1, 1000);
    Wait([&] { return sound.Snapshot().cleanup_failed; });
    assert(sound.Snapshot().active && coordinator.Mode() == AudioSessionMode::kFault && output.releases == 0);
    sound.Poll(1, 7000); assert(output.closes == 1);
    const auto recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
    assert(recording && !sound.StopForRecording(recording.generation, 10));
    output.close_ok = true;
    assert(sound.StopForRecording(recording.generation, 2000));
    Wait([&] { return output.releases == 1; });
    assert(!sound.Snapshot().active && sound.Snapshot().failed && !sound.Snapshot().cleanup_failed);
    assert(coordinator.IsCurrent(recording.generation) && coordinator.Complete(recording));
    std::puts("PASS: preparation/write errors remain errors and failed cleanup holds the lease until a checked retry");
}
void TestDismissalAndInvalidConfiguration() {
    AudioSessionCoordinator coordinator;
    Output output; output.blocked = true;
    WatchAlertSound sound(coordinator, output.Hooks(coordinator), 16000);
    sound.Poll(1, 1000); output.WaitEntered();
    sound.Silence();
    assert(sound.Snapshot().active && coordinator.BlocksSleep());
    output.Unblock(); Wait([&] { return output.releases == 1; });
    assert(output.writes == 1 && output.closes == 1 && !coordinator.BlocksSleep());
    sound.Poll(1, 7000); assert(output.prepares == 1);
    for (uint32_t rate : {0U, 7999U, 44100U, 48001U}) {
        Output invalid_output;
        WatchAlertSound invalid(coordinator, invalid_output.Hooks(coordinator), rate);
        invalid.Poll(1, 1);
        assert(invalid.Snapshot().failed && !invalid.Snapshot().active && invalid_output.prepares == 0);
    }
    std::puts("PASS: dismissal remains pending through in-flight output; invalid configuration never acquires audio");
}
}
int main() {
    TestToneAndRepeatWindow(); TestBusySessionsAndExpiredAlerts(); TestRecordingWaitsForBlockedOutput();
    TestFailuresAndCleanupRetry(); TestDismissalAndInvalidConfiguration();
}
