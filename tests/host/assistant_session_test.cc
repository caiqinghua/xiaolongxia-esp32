#include "audio/assistant_session_lifecycle.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

struct Fixture {
    AudioSessionCoordinator coordinator;
    bool network_released = true;
    bool audio_released = true;
    bool audio_ready = true;
    unsigned int resumes = 0;
    std::vector<int> calls;
    AssistantSessionLifecycle session{coordinator,
        [this](uint32_t) { calls.push_back(1); return network_released; },
        [this](uint32_t) { calls.push_back(2); return audio_released; },
        [this]() { ++resumes; return audio_ready; }};
};

void TestAssistantLifecycle() {
    Fixture f;
    const auto token = f.session.Begin();
    Check(token != 0 && f.session.IsActive(token), "assistant reserves before connecting");
    Check(f.coordinator.Mode() == AudioSessionMode::kAssistantConnecting, "connecting tracked");
    Check(f.coordinator.BlocksSleep(), "connection attempt blocks sleep");
    for (auto mode : {AudioSessionMode::kAssistantListening, AudioSessionMode::kAssistantThinking,
                      AudioSessionMode::kAssistantSpeaking, AudioSessionMode::kAssistantListening}) {
        Check(f.session.Update(mode, token), "assistant phase updated");
        Check(f.coordinator.Mode() == mode && f.coordinator.BlocksSleep(), "thinking and playback retain lease");
    }
    Check(f.session.Begin() == 0, "second session cannot start over active session");
    Check(f.session.Stop(token), "session stops after both resources released");
    Check(f.calls == std::vector<int>({1, 2}), "transport closes before audio cleanup");
    Check(!f.coordinator.BlocksSleep() && !f.session.IsActive(token), "idle only after cleanup");
    Check(f.resumes == 2, "audio admission restored once after cleanup");
    Check(!f.session.Stop(token) && !f.session.Poll(), "duplicate stop performs no work");
}

void TestPendingTransportAndAudio() {
    Fixture f;
    const auto token = f.session.Begin();
    f.network_released = false;
    Check(!f.session.Stop(token), "network timeout retains reservation");
    const auto cleanup_generation = f.coordinator.Generation();
    Check(f.session.IsClosing() && !f.session.IsActive(token), "closing rejects old callbacks");
    Check(f.calls == std::vector<int>({1}), "audio cleanup waits for transport exit");
    Check(f.session.Begin() == 0 && f.coordinator.BlocksSleep(), "closing prevents reconnect and sleep");
    Check(!f.coordinator.Begin(AudioSessionMode::kSleeping), "sleep reservation rejected during cleanup");
    f.network_released = true;
    f.audio_released = false;
    Check(!f.session.Poll(), "audio drain timeout remains pending");
    Check(f.coordinator.Generation() == cleanup_generation && f.resumes == 1, "cleanup retries reuse reservation");
    f.audio_released = true;
    Check(f.session.Poll() && !f.session.IsClosing(), "owner can finish pending cleanup later");
    Check(f.coordinator.Mode() == AudioSessionMode::kIdle && f.resumes == 2, "cleanup restores admission once");
}

void TestRecordingPreemption() {
    Fixture f;
    const auto token = f.session.Begin();
    f.session.Update(AudioSessionMode::kAssistantSpeaking, token);
    const auto recording = f.coordinator.Begin(AudioSessionMode::kLocalRecording);
    Check(static_cast<bool>(recording), "recording can preempt active assistant");
    Check(!f.session.IsActive(token) && !f.session.Update(AudioSessionMode::kAssistantListening, token),
          "preemption immediately rejects old assistant actions");
    Check(f.session.Stop(token), "old assistant token retired");
    Check(f.calls.empty() && f.resumes == 1, "preempted assistant does not touch new owner's resources");
    Check(f.coordinator.Complete(recording), "recording owner retains valid reservation");
    Check(f.session.Begin() == 0 && f.coordinator.Mode() == AudioSessionMode::kLocalRecording,
          "assistant cannot replace recording");
}

void TestStaleActions() {
    Fixture f;
    const auto first = f.session.Begin();
    Check(f.session.Stop(first), "first connection closes");
    const auto second = f.session.Begin();
    Check(second != first && second != 0, "new connection has fresh generation");
    const auto calls = f.calls.size();
    Check(!f.session.Stop(first), "old close cannot terminate new connection");
    Check(!f.session.Update(AudioSessionMode::kAssistantSpeaking, first), "old TTS cannot change new connection");
    Check(f.calls.size() == calls && f.session.IsActive(second), "new connection remains untouched");
    Check(!f.coordinator.BeginAssistantCleanup(first), "coordinator also rejects stale cleanup atomically");
    Check(!f.session.Update(AudioSessionMode::kLocalRecording, second), "assistant cannot claim another purpose");
    Check(f.session.Stop(second), "second connection closes normally");
}

void TestRecordingPreemptsAssistantCleanup() {
    for (bool network_pending : {false, true}) {
        Fixture f;
        const auto token = f.session.Begin();
        f.network_released = !network_pending;
        f.audio_released = false;
        Check(!f.session.Stop(token), "assistant teardown remains pending");
        const auto recording = f.coordinator.Begin(AudioSessionMode::kLocalRecording);
        Check(static_cast<bool>(recording), "recording can take over pending assistant teardown");
        const auto calls = f.calls.size();
        Check(f.session.Poll() && !f.session.Token(), "preempted cleanup retires its old intent");
        Check(f.calls.size() == calls && f.resumes == 1, "old cleanup cannot resume or quiesce new owner");
        Check(f.coordinator.BlocksSleep() && !f.coordinator.AllowsAssistantResources(),
            "recording reservation retains unresolved resources");
        Check(f.coordinator.Complete(recording), "recording owner completes its own handoff");
    }
}

void TestRecordingPreemptsDuringCloseCallback() {
    for (bool closed : {false, true}) {
        AudioSessionCoordinator coordinator;
        AudioSessionTransition recording;
        unsigned drains = 0, resumes = 0;
        AssistantSessionLifecycle session(coordinator, [&](uint32_t) {
            recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
            Check(static_cast<bool>(recording), "recording reserves while transport close is in progress");
            return closed;
        }, [&](uint32_t) { ++drains; return true; }, [&]() { ++resumes; return true; });
        const auto token = session.Begin();
        Check(session.Stop(token) && !session.Token(), "close completion observes recording preemption");
        Check(drains == 0 && resumes == 1, "old close cannot continue audio cleanup after handoff");
        Check(coordinator.IsCurrent(recording.generation) && coordinator.BlocksSleep(),
            "recording remains responsible for unfinished resources");
    }
}

void TestAudioStartupFailure() {
    Fixture f;
    f.audio_ready = false;
    f.audio_released = false;
    Check(f.session.Begin() == 0 && f.session.IsClosing(), "audio resume failure initiates checked cleanup");
    Check(f.coordinator.BlocksSleep(), "failed admission does not release unknown resources");
    f.audio_ready = true;
    f.audio_released = true;
    Check(f.session.Poll(), "failed startup can recover");
    const auto token = f.session.Begin();
    Check(token != 0 && f.session.Stop(token), "new startup succeeds after recovery");
    AudioSessionCoordinator coordinator;
    AssistantSessionLifecycle missing(coordinator, {}, {}, {});
    Check(missing.Begin() == 0 && !coordinator.BlocksSleep(), "missing callbacks cannot reserve hardware");
}

void TestPreemptionDuringStartup() {
    AudioSessionCoordinator coordinator;
    AudioSessionTransition recording;
    unsigned int closes = 0;
    AssistantSessionLifecycle session(coordinator,
        [&](uint32_t) { ++closes; return true; }, [](uint32_t) { return true; }, [&]() {
            recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
            return true;
        });
    Check(session.Begin() == 0, "preempted audio admission cannot report assistant ready");
    Check(closes == 0 && coordinator.Complete(recording), "new recording owner is not cleaned up by assistant");
    Check(!session.Token() && !session.IsClosing(), "preempted startup leaves no stale session intent");
}

void TestNextTurnKeepsTransportAndDrainsAudio() {
    Fixture f;
    const auto first = f.session.Begin();
    f.session.Update(AudioSessionMode::kAssistantSpeaking, first);
    f.audio_released = false;
    Check(f.session.BeginNextTurn(first), "next turn revokes previous audio lease");
    const auto second = f.session.Token();
    Check(second != first && f.session.IsTurning(), "turn owns a new pending lease");
    Check(!f.session.IsActive(first) && !f.session.IsActive(second), "neither turn captures during drain");
    Check(!f.session.Update(AudioSessionMode::kAssistantListening, second), "cannot skip audio drain");
    Check(!f.session.BeginNextTurn(second) && !f.session.Stop(first), "stale and duplicate actions do no work");
    Check(!f.session.PollNextTurn(), "in-flight audio keeps turn pending");
    Check(f.calls == std::vector<int>({2}) && f.resumes == 1, "transport stays open and audio is not resumed early");
    Check(f.coordinator.BlocksSleep(), "pending turn retains power reservation");
    f.audio_released = true;
    Check(f.session.PollNextTurn() && f.session.IsActive(second), "capture resumes only after checked drain");
    Check(f.calls == std::vector<int>({2, 2}) && f.resumes == 2, "successful turn has no physical reconnect");
    Check(f.session.Stop(second), "final close still releases both resources");
    Check(f.calls == std::vector<int>({2, 2, 1, 2}), "final teardown preserves transport-before-audio order");
}

void TestNextTurnCanBeCanceledAndPreempted() {
    for (bool preempt : {false, true}) {
        Fixture f;
        const auto first = f.session.Begin();
        Check(f.session.BeginNextTurn(first), "turn begins");
        const auto second = f.session.Token();
        if (preempt) {
            const auto recording = f.coordinator.Begin(AudioSessionMode::kLocalRecording);
            Check(static_cast<bool>(recording), "recording preempts pending turn");
            Check(f.session.RetireIfPreempted(), "preempted turn clears intent");
            Check(!f.session.IsTurning() && !f.session.PollNextTurn() && !f.session.Token(), "old turn cannot resume");
            Check(f.calls.empty() && f.coordinator.Complete(recording), "recording owns cleanup");
        } else {
            f.network_released = false;
            Check(!f.session.Stop(second) && f.session.IsClosing(), "cancel waits for transport shutdown");
            Check(!f.session.IsTurning() && !f.session.PollNextTurn(), "canceled turn never resumes");
            f.network_released = true;
            Check(f.session.Poll() && !f.coordinator.BlocksSleep(), "canceled turn finishes cleanup");
        }
    }
}

void TestNextTurnResumeFailure() {
    Fixture f;
    const auto first = f.session.Begin();
    Check(f.session.BeginNextTurn(first), "turn begins before resume failure");
    f.audio_ready = false;
    f.network_released = false;
    Check(!f.session.PollNextTurn() && f.session.IsClosing(), "failed resume falls back to checked physical close");
    Check(f.coordinator.BlocksSleep(), "failed resume keeps resources reserved");
    f.network_released = f.audio_ready = true;
    Check(f.session.Poll(), "failed turn eventually retires");
}
}  // namespace

int main() {
    TestAssistantLifecycle();
    TestPendingTransportAndAudio();
    TestRecordingPreemption();
    TestRecordingPreemptsAssistantCleanup();
    TestRecordingPreemptsDuringCloseCallback();
    TestStaleActions();
    TestAudioStartupFailure();
    TestPreemptionDuringStartup();
    TestNextTurnKeepsTransportAndDrainsAudio();
    TestNextTurnCanBeCanceledAndPreempted();
    TestNextTurnResumeFailure();
    std::cout << "11 assistant session lifecycle tests passed\n";
}
