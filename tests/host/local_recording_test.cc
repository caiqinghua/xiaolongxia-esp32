#include "audio/local_recording_service.h"
#include "audio/assistant_session_lifecycle.h"
#include "audio/audio_service.h"
#include "esp_timer.h"
#include "storage/sd_card_storage.h"
#include "system/audream_power_manager.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
void TestPauseTimeoutAndStopRetry() {
    AudioSessionCoordinator coordinator;
    AudioService audio;
    audio.coordinator = &coordinator;
    auto& recorder = LocalRecordingService::GetInstance();
    Check(recorder.Start(audio), "recording starts");
    Check(recorder.GetSnapshot().state == LocalRecordingService::State::kRecording, "snapshot reports actual capture state");
    audio.EmitPacket();
    test_time_us += 2000000;
    audio.drain_succeeds = false;
    Check(!recorder.Pause(audio), "pause timeout is not success");
    Check(recorder.GetSnapshot().state == LocalRecordingService::State::kPausing, "snapshot does not hide a pending pause");
    Check(recorder.IsRecording() && !recorder.IsPaused(), "pausing is not paused");
    Check(!recorder.Resume(audio), "resume cannot bypass pending pause");
    Check(!recorder.Stop(audio).valid, "stop timeout is not success");
    const auto pending = recorder.GetSnapshot();
    Check(pending.state == LocalRecordingService::State::kStopping && pending.duration_ms == 2000,
        "snapshot reports pending stop and frozen duration atomically");
    Check(coordinator.BlocksSleep(), "timeout retains session lease");
    Check(AudreamPowerManager::GetInstance().recording_active, "timeout retains power blocker");
    Check(static_cast<bool>(audio.callback), "writer callback retained for queued packet");
    audio.EmitPacket();
    test_time_us += 5000000;
    audio.drain_succeeds = true;
    const auto result = recorder.Stop(audio);
    Check(result.valid && result.packet_count == 2, "drain retry saves all accepted packets");
    Check(result.duration_ms == 2000, "drain wait excluded from duration");
    const auto closed = recorder.GetSnapshot();
    Check(closed.state == LocalRecordingService::State::kClosed && closed.last.valid &&
        closed.last.recording_id == result.recording_id && closed.last.duration_ms == 2000,
        "saved snapshot identifies only committed metadata");
    Check(!recorder.IsRecording() && !coordinator.BlocksSleep(), "successful close releases session");
    Check(!audio.callback && audio.resumed_assistant, "successful close releases audio");
    Check(!AudreamPowerManager::GetInstance().recording_active, "successful close releases power blocker");
    recorder.ForgetDeletedRecording("another-recording");
    Check(recorder.GetSnapshot().last.valid, "deleting another recording preserves the last saved receipt");
    recorder.ForgetDeletedRecording(result.recording_id);
    Check(!recorder.GetSnapshot().last.valid, "deleted recording is no longer presented as a saved local copy");
}

void TestMetadataFailureRetainsLease() {
    AudioSessionCoordinator coordinator;
    AudioService audio;
    audio.coordinator = &coordinator;
    auto& recorder = LocalRecordingService::GetInstance();
    Check(recorder.Start(audio), "metadata test starts");
    audio.EmitPacket();
    const auto path = recorder.CurrentMetadata().file_path + ".json";
    std::filesystem::create_directory(path);
    // A directory at the destination makes atomic metadata rename fail.
    Check(!recorder.Stop(audio).valid, "metadata failure reported");
    Check(recorder.IsRecording() && coordinator.BlocksSleep(), "metadata failure retains stop lease");
    Check(!std::filesystem::exists(path + ".tmp"), "failed metadata staging cleaned");
    std::filesystem::remove(path);
    const auto result = recorder.Stop(audio);
    Check(result.valid && result.packet_count == 1, "metadata retry preserves packet count");
    std::ifstream file(path);
    const std::string json{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    Check(json.find(result.recording_id) != std::string::npos, "metadata committed for correct recording");
    Check(!coordinator.BlocksSleep(), "metadata commit releases lease");
}

void TestFailedPacketCannotBecomeSavedOnMetadataRetry() {
    AudioSessionCoordinator coordinator;
    AudioService audio; audio.coordinator = &coordinator;
    auto& recorder = LocalRecordingService::GetInstance();
    Check(recorder.Start(audio), "failed packet test starts");
    audio.EmitPacket();
    Check(static_cast<bool>(audio.callback), "recording error callback installed");
    audio.callback(nullptr, 0);
    const auto metadata = recorder.CurrentMetadata();
    const auto path = metadata.file_path + ".json";
    std::filesystem::create_directory(path);
    Check(!recorder.Stop(audio).valid && recorder.IsRecording() && coordinator.BlocksSleep(),
        "failed metadata commit retains cleanup even after packet write error");
    std::filesystem::remove(path);
    const auto result = recorder.Stop(audio);
    Check(!result.valid && result.recording_id == metadata.recording_id && !recorder.IsRecording() && !coordinator.BlocksSleep(),
        "closed failed file retires resources without claiming successful audio");
    std::ifstream file(path);
    const std::string json{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    Check(json.find("\"recording_complete\":false") != std::string::npos && std::filesystem::exists(metadata.file_path),
        "failure is persisted and existing audio is retained for inspection");
    Check(!recorder.Stop(audio).valid, "repeated stop cannot turn failed save into success");
    Check(recorder.Start(audio), "next recording can start after actual cleanup");
    audio.EmitPacket();
    Check(recorder.Stop(audio).valid, "new recording resets failure state");
}

void TestResumeFailureCanStop() {
    AudioSessionCoordinator coordinator;
    AudioService audio;
    audio.coordinator = &coordinator;
    auto& recorder = LocalRecordingService::GetInstance();
    Check(recorder.Start(audio), "resume test starts");
    audio.EmitPacket();
    Check(recorder.Pause(audio), "pause succeeds");
    audio.start_succeeds = false;
    Check(!recorder.Resume(audio), "failed codec restart reported");
    Check(recorder.IsPaused() && coordinator.BlocksSleep(), "failed resume retains paused recording");
    Check(recorder.Stop(audio).valid, "failed resume can be finalized safely");
    Check(!coordinator.BlocksSleep(), "fault cleanup releases after file close");
}

void TestStartFailureAndLegacyRecorder() {
    AudioSessionCoordinator coordinator;
    AudioService audio;
    audio.coordinator = &coordinator;
    audio.start_succeeds = false;
    auto& recorder = LocalRecordingService::GetInstance();
    Check(!recorder.Start(audio), "failed codec start reported");
    Check(!recorder.IsRecording() && !audio.callback, "failed start cleans writer callback");
    Check(!coordinator.BlocksSleep(), "confirmed clean start rollback releases lease");
    Check(audio.resumed_assistant, "confirmed start rollback permits assistant audio again");
    audio.coordinator = nullptr;
    audio.start_succeeds = true;
    Check(recorder.Start(audio), "recording-only profile needs no coordinator");
    audio.EmitPacket();
    Check(recorder.Pause(audio), "legacy pause");
    Check(recorder.Resume(audio), "legacy resume");
    Check(recorder.Stop(audio).valid, "legacy recording saved");
}

void TestTransportHandoffFailureAndRecovery() {
    AudioSessionCoordinator coordinator;
    AudioService audio;
    audio.coordinator = &coordinator;
    auto& recorder = LocalRecordingService::GetInstance();
    bool transport_released = false;
    int attempts = 0;
    Check(recorder.SetAssistantHandoff([&](uint64_t generation, uint32_t) {
        ++attempts;
        Check(coordinator.IsCurrent(generation), "handoff has current reservation");
        Check(!coordinator.AllowsAssistantResources(), "handoff blocks assistant restart");
        Check(!audio.capturing && !audio.callback, "handoff precedes codec and writer callback");
        return transport_released;
    }), "install handoff");
    const auto before = std::distance(std::filesystem::directory_iterator(SdCardStorage::GetInstance().directory),
                                      std::filesystem::directory_iterator{});
    Check(!recorder.Start(audio), "transport timeout rejects recording start");
    Check(!recorder.IsRecording() && !audio.capturing, "failed preflight is not recording");
    Check(coordinator.Mode() == AudioSessionMode::kFault, "timeout retains fault lease");
    Check(!recorder.SetAssistantHandoff(nullptr), "cannot remove pending handoff");
    Check(!recorder.RecoverFailedStart(audio), "unreleased transport cannot recover");
    Check(!coordinator.Begin(AudioSessionMode::kAssistantConnecting), "fault prevents a new assistant");
    const auto after = std::distance(std::filesystem::directory_iterator(SdCardStorage::GetInstance().directory),
                                     std::filesystem::directory_iterator{});
    Check(before == after, "preflight failure creates no empty recording file");
    transport_released = true;
    Check(recorder.RecoverFailedStart(audio), "matching fault recovers after release");
    Check(!coordinator.BlocksSleep() && audio.resumed_assistant, "recovery releases reservation");
    Check(recorder.Start(audio), "start succeeds after recovery");
    Check(attempts == 4, "handoff reruns for new recording lease");
    audio.EmitPacket();
    Check(recorder.Stop(audio).valid, "recovered recording saves");
    Check(recorder.SetAssistantHandoff(nullptr), "restore legacy test setup");
}

void TestStorageFailureDoesNotPreemptAssistant() {
    AudioSessionCoordinator coordinator;
    AudioService audio;
    audio.coordinator = &coordinator;
    const auto assistant = coordinator.Begin(AudioSessionMode::kAssistantConnecting);
    Check(coordinator.Complete(assistant), "assistant owns resources");
    auto& recorder = LocalRecordingService::GetInstance();
    Check(recorder.SetAssistantHandoff([](uint64_t, uint32_t) {
        Check(false, "storage failure must not close assistant");
        return false;
    }), "install storage test handoff");
    SdCardStorage::GetInstance().status.mounted = false;
    Check(!recorder.Start(audio), "unavailable storage rejects recording");
    SdCardStorage::GetInstance().status.mounted = true;
    Check(coordinator.IsCurrent(assistant.generation) &&
        coordinator.Mode() == AudioSessionMode::kAssistantConnecting, "assistant lease unchanged");
    Check(recorder.SetAssistantHandoff(nullptr), "remove storage test handoff");
}

void TestRecoveryCannotClaimAnotherFault() {
    AudioSessionCoordinator coordinator;
    AudioService audio;
    audio.coordinator = &coordinator;
    auto& recorder = LocalRecordingService::GetInstance();
    int calls = 0;
    Check(recorder.SetAssistantHandoff([&](uint64_t, uint32_t) { ++calls; return false; }), "install failed handoff");
    Check(!recorder.Start(audio), "create recording start fault");
    Check(coordinator.RecoverToIdle(true), "explicit external recovery");
    const auto unrelated = coordinator.Begin(AudioSessionMode::kMaintenance);
    Check(coordinator.Fail(unrelated, false), "unrelated maintenance fault");
    Check(!recorder.RecoverFailedStart(audio), "stale recording cleanup rejected");
    Check(calls == 1 && coordinator.IsCurrent(unrelated.generation) &&
        coordinator.Mode() == AudioSessionMode::kFault, "unrelated fault untouched");
    Check(recorder.SetAssistantHandoff(nullptr), "remove stale handoff");
}
void TestRecordingWaitsForPlaybackCleanup() {
    auto& recorder = LocalRecordingService::GetInstance();
    for (int phase = 0; phase < 4; ++phase) {
        AudioSessionCoordinator coordinator;
        AudioService audio; audio.coordinator = &coordinator;
        const auto playback = coordinator.Begin(AudioSessionMode::kLocalPlayback);
        if (phase == 1 || phase == 2) Check(coordinator.Complete(playback), "playback ready for recording preemption");
        if (phase == 2) Check(coordinator.SetLocalPlaybackPaused(playback.generation, true), "playback paused");
        if (phase == 3) Check(coordinator.Fail(playback, false), "playback start failed with retained resources");
        bool released = false;
        unsigned calls = 0;
        Check(recorder.SetPlaybackHandoff([&](uint64_t generation, uint32_t) {
            ++calls;
            Check(!audio.capturing && !audio.callback, "playback cleanup happens before opening microphone or writer callback");
            Check(coordinator.HoldsExclusiveResources(generation), "cleanup holds the new recording reservation");
            Check(!coordinator.ReleaseLocalPlayback(playback.generation), "old playback cleanup cannot clear the recording owner");
            return released;
        }), "install playback cleanup handoff");
        const auto before = std::distance(std::filesystem::directory_iterator(SdCardStorage::GetInstance().directory),
            std::filesystem::directory_iterator{});
        Check(!recorder.Start(audio) && !audio.capturing, "unfinished playback prevents capture");
        Check(coordinator.Mode() == AudioSessionMode::kFault && coordinator.BlocksSleep(), "failed playback handoff retains cleanup lease");
        Check(!recorder.SetPlaybackHandoff(nullptr), "pending cleanup callback cannot be removed");
        Check(!recorder.RecoverFailedStart(audio), "retry does not fake playback cleanup success");
        const auto after = std::distance(std::filesystem::directory_iterator(SdCardStorage::GetInstance().directory),
            std::filesystem::directory_iterator{});
        Check(before == after, "failed playback handoff creates no empty recording");
        released = true;
        Check(recorder.RecoverFailedStart(audio) && !coordinator.BlocksSleep(), "actual playback release permits fault recovery");
        Check(recorder.Start(audio), "capture starts after actual cleanup");
        Check(calls == 3, "idle recording does not repeat obsolete playback cleanup");
        audio.EmitPacket(); Check(recorder.Stop(audio).valid, "recording following playback interruption saves normally");
        Check(recorder.SetPlaybackHandoff(nullptr), "remove released playback callback");
    }
}
void TestMissingPlaybackHandoffFailsClosed() {
    auto& recorder = LocalRecordingService::GetInstance();
    AudioSessionCoordinator coordinator; AudioService audio; audio.coordinator = &coordinator;
    const auto playback = coordinator.Begin(AudioSessionMode::kLocalPlayback);
    Check(coordinator.Complete(playback), "playback setup");
    Check(!recorder.Start(audio) && !audio.capturing, "missing playback cleanup cannot fall back to assistant-only drain");
    Check(!recorder.RecoverFailedStart(audio), "missing handoff retains resources on retry");
    Check(recorder.SetPlaybackHandoff([](uint64_t, uint32_t) { return true; }), "missing cleanup owner can be registered for explicit recovery");
    Check(recorder.RecoverFailedStart(audio), "registered owner confirms actual release");
    Check(recorder.SetPlaybackHandoff(nullptr), "restore normal recording setup");
}
void TestRecordingTakesOverPendingAssistantCleanup() {
    auto& recorder = LocalRecordingService::GetInstance();
    for (bool network_pending : {false, true}) {
        for (bool ready_on_first_attempt : {false, true}) {
            AudioSessionCoordinator coordinator;
            AudioService audio; audio.coordinator = &coordinator;
            bool transport_released = !network_pending;
            audio.quiesce_succeeds = false;
            unsigned resumes = 0;
            AssistantSessionLifecycle assistant(coordinator,
                [&](uint32_t) { return transport_released; },
                [&](uint32_t timeout) { return audio.QuiesceAssistant(timeout); },
                [&]() { ++resumes; return audio.ResumeAssistantAudio(); });
            const auto token = assistant.Begin();
            Check(token && !assistant.Stop(token), "assistant close waits for outstanding resources");
            bool handoff_ready = ready_on_first_attempt;
            unsigned handoffs = 0;
            Check(recorder.SetAssistantHandoff([&](uint64_t generation, uint32_t timeout) {
                ++handoffs;
                Check(coordinator.HoldsExclusiveResources(generation), "recording owns the cleanup reservation");
                Check(!audio.capturing && !audio.callback, "cleanup precedes recording capture and writer setup");
                assistant.RetireIfPreempted();
                Check(!assistant.Token() && !assistant.Poll() && resumes == 1,
                    "old assistant cannot restore audio admission during recording handoff");
                transport_released = audio.quiesce_succeeds = handoff_ready;
                return transport_released && audio.QuiesceAssistant(timeout);
            }), "install production recording handoff entry point");
            const auto before = std::distance(std::filesystem::directory_iterator(SdCardStorage::GetInstance().directory),
                std::filesystem::directory_iterator{});
            if (!ready_on_first_attempt) {
                Check(!recorder.Start(audio) && handoffs == 1 && !audio.capturing,
                    "unreleased assistant prevents capture, not cleanup ownership");
                Check(coordinator.Mode() == AudioSessionMode::kFault && coordinator.BlocksSleep(),
                    "failed handoff retains a recording fault lease");
                Check(before == std::distance(std::filesystem::directory_iterator(SdCardStorage::GetInstance().directory),
                    std::filesystem::directory_iterator{}), "failed takeover creates no empty audio file");
                handoff_ready = true;
                Check(recorder.RecoverFailedStart(audio), "recording owner can finish failed takeover");
            }
            Check(recorder.Start(audio) && audio.capturing, "recording starts after assistant resources are released");
            Check(!assistant.Stop(token) && !assistant.Poll() && resumes == 1,
                "late assistant cleanup cannot disturb active recording");
            audio.EmitPacket();
            const auto result = recorder.Stop(audio);
            Check(result.valid && result.packet_count == 1 && !coordinator.BlocksSleep(),
                "recording saves and releases resources after takeover");
            Check(recorder.SetAssistantHandoff(nullptr), "remove released test handoff");
            const auto next = assistant.Begin();
            Check(next && next != token && assistant.Stop(next), "assistant can restart after recording finishes");
        }
    }
}
}  // namespace

int main(int argc, char** argv) {
    Check(argc == 2, "temporary recording directory required");
    SdCardStorage::GetInstance().directory = argv[1];
    std::filesystem::create_directories(argv[1]);
    TestPauseTimeoutAndStopRetry();
    TestMetadataFailureRetainsLease();
    TestFailedPacketCannotBecomeSavedOnMetadataRetry();
    TestResumeFailureCanStop();
    TestStartFailureAndLegacyRecorder();
    TestTransportHandoffFailureAndRecovery();
    TestStorageFailureDoesNotPreemptAssistant();
    TestRecoveryCannotClaimAnotherFault();
    TestRecordingWaitsForPlaybackCleanup(); TestMissingPlaybackHandoffFailsClosed();
    TestRecordingTakesOverPendingAssistantCleanup();
    std::cout << "11 local recording lifecycle tests passed\n";
}
