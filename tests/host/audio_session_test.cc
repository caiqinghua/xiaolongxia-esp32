#include "audio_session_coordinator.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void TestRecordingPreemptsAssistant() {
    AudioSessionCoordinator coordinator;
    Check(!coordinator.BlocksSleep(), "idle has no lease");
    const auto assistant = coordinator.Begin(AudioSessionMode::kAssistantConnecting);
    Check(static_cast<bool>(assistant), "assistant reserve");
    Check(coordinator.BlocksSleep(), "connection transition blocks sleep");
    Check(!coordinator.AllowsAssistantResources(), "transition blocks unowned audio");
    Check(coordinator.Complete(assistant), "assistant connected");
    Check(coordinator.UpdateAssistant(AudioSessionMode::kAssistantSpeaking, assistant.generation), "assistant speaking");
    const auto recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
    Check(static_cast<bool>(recording), "recording preempts speech");
    Check(!coordinator.EndAssistant(assistant.generation), "stale close cannot release recording");
    Check(!coordinator.UpdateAssistant(AudioSessionMode::kAssistantListening, assistant.generation), "stale TTS cannot resume microphone");
    Check(coordinator.Complete(recording), "recording started after drain");
    Check(!coordinator.AllowsAssistantResources(), "recording rejects assistant resources");
    Check(!coordinator.Begin(AudioSessionMode::kFileTransfer), "transfer cannot preempt recording");
    Check(!coordinator.Begin(AudioSessionMode::kMaintenance), "OTA cannot preempt recording");
    Check(!coordinator.Begin(AudioSessionMode::kSleeping), "sleep cannot preempt recording");
}

void TestPauseRetainsLease() {
    AudioSessionCoordinator coordinator;
    Check(coordinator.Complete(coordinator.Begin(AudioSessionMode::kLocalRecording)), "recording reserve");
    Check(coordinator.Complete(coordinator.Begin(AudioSessionMode::kRecordingPaused)), "pause completed");
    Check(coordinator.BlocksSleep(), "pause blocks sleep");
    Check(!coordinator.Begin(AudioSessionMode::kAssistantConnecting), "pause rejects assistant");
    Check(!coordinator.Begin(AudioSessionMode::kFileTransfer), "pause rejects transfer");
    Check(coordinator.Complete(coordinator.Begin(AudioSessionMode::kLocalRecording)), "resume same lease");
    Check(coordinator.Complete(coordinator.Begin(AudioSessionMode::kIdle)), "stop after file close");
    Check(!coordinator.BlocksSleep(), "stop releases lease");
}

void TestFailedDrainRetainsLease() {
    AudioSessionCoordinator coordinator;
    const auto transition = coordinator.Begin(AudioSessionMode::kLocalRecording);
    Check(coordinator.Fail(transition, false), "failed drain recorded");
    Check(coordinator.Mode() == AudioSessionMode::kFault, "fault retained");
    Check(coordinator.BlocksSleep(), "fault blocks sleep");
    Check(!coordinator.AllowsAssistantResources(), "fault blocks assistant");
    Check(!coordinator.Begin(AudioSessionMode::kIdle), "fault cannot bypass recovery");
    Check(!coordinator.RecoverToIdle(false), "unreleased resources cannot recover");
    Check(coordinator.RecoverToIdle(true), "confirmed recovery");
    Check(!coordinator.Complete(transition), "late completion cannot reenter recording");
}

void TestStaleTransitionRejected() {
    AudioSessionCoordinator coordinator;
    const auto old = coordinator.Begin(AudioSessionMode::kLocalRecording);
    Check(coordinator.Fail(old, true), "failed start clean rollback");
    const auto current = coordinator.Begin(AudioSessionMode::kLocalRecording);
    Check(!coordinator.Complete(old), "old start completion rejected");
    Check(!coordinator.Fail(old, true), "old failure rejected");
    Check(coordinator.Complete(current), "current completion accepted");
    Check(!coordinator.Complete(current), "duplicate completion rejected");
}

void TestConcurrentReservation() {
    AudioSessionCoordinator coordinator;
    std::atomic<int> winners{0};
    std::vector<std::thread> contenders;
    for (int i = 0; i < 16; ++i) {
        contenders.emplace_back([&]() {
            if (coordinator.Begin(AudioSessionMode::kLocalRecording)) {
                ++winners;
            }
        });
    }
    for (auto& contender : contenders) {
        contender.join();
    }
    Check(winners == 1, "exactly one owner across button and BLE commands");
}

void TestMaintenanceAndTransfer() {
    AudioSessionCoordinator coordinator;
    Check(coordinator.Complete(coordinator.Begin(AudioSessionMode::kMaintenance)), "maintenance reserve");
    Check(!coordinator.Begin(AudioSessionMode::kLocalRecording), "firmware write cannot be interrupted");
    Check(coordinator.Complete(coordinator.Begin(AudioSessionMode::kIdle)), "maintenance done");
    Check(coordinator.Complete(coordinator.Begin(AudioSessionMode::kFileTransfer)), "transfer reserve");
    const auto recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
    Check(recording.previous == AudioSessionMode::kFileTransfer, "recording requests transfer teardown");
    Check(coordinator.BlocksSleep(), "transfer teardown blocks sleep");
    Check(coordinator.Complete(recording), "recording starts after transfer teardown");
}

void TestTransferReleaseChecksOwnerAndPurpose() {
    AudioSessionCoordinator coordinator;
    auto transfer = coordinator.Begin(AudioSessionMode::kFileTransfer);
    Check(coordinator.ReleaseFileTransfer(transfer.generation), "pending transfer rollback is checked");
    transfer = coordinator.Begin(AudioSessionMode::kFileTransfer);
    Check(coordinator.Fail(transfer, false), "transfer cleanup fault retained");
    Check(coordinator.ReleaseFileTransfer(transfer.generation), "confirmed transfer fault cleanup releases lease");
    const auto assistant = coordinator.Begin(AudioSessionMode::kAssistantConnecting);
    Check(coordinator.Complete(assistant), "assistant owns audio");
    Check(!coordinator.ReleaseFileTransfer(assistant.generation), "transfer cleanup cannot release assistant audio");
    Check(coordinator.EndAssistant(assistant.generation), "assistant cleanup done");
    const auto recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
    Check(coordinator.Fail(recording, false), "recording fault retained");
    Check(!coordinator.ReleaseFileTransfer(recording.generation), "transfer cleanup cannot clear a recording fault");
    Check(!coordinator.Begin(AudioSessionMode::kLocalRecording), "recording fault requires explicit recovery");
}

void TestRecordingPreemptsPendingTransfer() {
    AudioSessionCoordinator coordinator;
    for (bool failed : {false, true}) {
        const auto transfer = coordinator.Begin(AudioSessionMode::kFileTransfer);
        if (failed) Check(coordinator.Fail(transfer, false), "transfer preparation failed");
        const auto recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
        Check(static_cast<bool>(recording), "recording inherits pending transfer teardown");
        Check(!coordinator.Complete(transfer), "stale transfer cannot become ready");
        Check(!coordinator.ReleaseFileTransfer(transfer.generation), "stale transfer cannot clear recording reservation");
        Check(!coordinator.ReleaseFileTransfer(recording.generation), "wrong purpose cannot clear recording reservation");
        Check(coordinator.Complete(recording), "recording completes after checked handoff");
        Check(coordinator.Complete(coordinator.Begin(AudioSessionMode::kIdle)), "recording stops");
    }
}
void TestLocalPlaybackIsolationAndPause() {
    AudioSessionCoordinator coordinator;
    const auto playback = coordinator.Begin(AudioSessionMode::kLocalPlayback);
    Check(playback && coordinator.BlocksSleep(), "playback preparation owns resources");
    Check(coordinator.OwnsLocalPlaybackPreparation(playback.generation), "preparation is checked atomically by purpose");
    Check(!coordinator.OwnsLocalPlaybackResources(playback.generation), "output cannot start during preparation");
    Check(coordinator.Complete(playback), "playback becomes ready");
    Check(!coordinator.OwnsLocalPlaybackPreparation(playback.generation), "active playback cannot prepare a second output");
    Check(coordinator.OwnsLocalPlaybackResources(playback.generation), "ready playback has a dedicated owner");
    Check(!coordinator.AllowsAssistantResources() && !coordinator.OwnsAssistantResources(playback.generation),
        "local playback never authorizes assistant or microphone resources");
    for (const auto mode : {AudioSessionMode::kAssistantConnecting, AudioSessionMode::kFileTransfer,
        AudioSessionMode::kMaintenance, AudioSessionMode::kSleeping, AudioSessionMode::kLocalPlayback})
        Check(!coordinator.Begin(mode), "playback excludes conflicting owners");
    Check(coordinator.SetLocalPlaybackPaused(playback.generation, true), "pause retains the same playback token");
    Check(coordinator.Mode() == AudioSessionMode::kPlaybackPaused && coordinator.BlocksSleep(), "paused file still owns resources");
    Check(!coordinator.Begin(AudioSessionMode::kLocalPlayback), "resume cannot create a second owner");
    Check(coordinator.SetLocalPlaybackPaused(playback.generation, false), "resume retains owner generation");
    Check(coordinator.ReleaseLocalPlayback(playback.generation), "confirmed cleanup releases playback");
    Check(!coordinator.ReleaseLocalPlayback(playback.generation) && !coordinator.SetLocalPlaybackPaused(playback.generation, false),
        "duplicate release and stale resume rejected");
    Check(!coordinator.BlocksSleep(), "closed playback no longer blocks sleep");
}
void TestRecordingPreemptsEveryPlaybackPhase() {
    for (int phase = 0; phase < 4; ++phase) {
        AudioSessionCoordinator coordinator;
        const auto playback = coordinator.Begin(AudioSessionMode::kLocalPlayback);
        if (phase == 1 || phase == 2) Check(coordinator.Complete(playback), "playback ready");
        if (phase == 2) Check(coordinator.SetLocalPlaybackPaused(playback.generation, true), "playback paused");
        if (phase == 3) Check(coordinator.Fail(playback, false), "playback cleanup fault retained");
        const auto recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
        Check(recording && AudioSessionCoordinator::IsLocalPlayback(recording.interrupted_purpose),
            "recording knows it must drain the playback owner in every phase");
        Check(!coordinator.ReleaseLocalPlayback(playback.generation) && !coordinator.ReleaseLocalPlayback(recording.generation),
            "old or wrong-purpose playback cleanup cannot release the recording reservation");
        Check(!coordinator.SetLocalPlaybackPaused(playback.generation, false), "late resume cannot revive the preempted owner");
        Check(!coordinator.OwnsLocalPlaybackResources(playback.generation), "preemption immediately fences old output");
        Check(!coordinator.OwnsLocalPlaybackPreparation(recording.generation), "recording reservation cannot open playback output");
        Check(!coordinator.MarkLocalPlaybackFault(playback.generation) && !coordinator.MarkLocalPlaybackFault(recording.generation),
            "late playback cleanup failure cannot fault a recording reservation");
        Check(coordinator.Complete(recording), "recording proceeds after checked cleanup");
    }
}
void TestPlaybackCannotReleaseAnotherFault() {
    AudioSessionCoordinator coordinator;
    auto playback = coordinator.Begin(AudioSessionMode::kLocalPlayback);
    Check(coordinator.MarkLocalPlaybackFault(playback.generation), "failed playback retains fault");
    Check(coordinator.Mode() == AudioSessionMode::kFault && !coordinator.OwnsLocalPlaybackPreparation(playback.generation),
        "cleanup fault retains resources without authorizing preparation");
    Check(coordinator.ReleaseLocalPlayback(playback.generation), "matching playback fault can close after actual drain");
    const auto recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
    Check(coordinator.Fail(recording, false), "recording fault retained");
    Check(!coordinator.ReleaseLocalPlayback(recording.generation), "playback cannot clear recording failure");
}
}  // namespace

void TestDevicePairingPreemption() {
    for (const auto target : {AudioSessionMode::kLocalRecording, AudioSessionMode::kFileTransfer, AudioSessionMode::kSleeping}) {
        AudioSessionCoordinator coordinator;
        const auto pairing = coordinator.Begin(AudioSessionMode::kDevicePairing);
        Check(coordinator.OwnsDevicePairingResources(pairing.generation), "pairing prepare owns its generation");
        Check(coordinator.Complete(pairing), "pairing reservation completed");
        Check(coordinator.BlocksSleep() && !coordinator.AllowsAssistantResources(), "pairing blocks unowned audio and idle sleep");
        Check(!coordinator.Begin(AudioSessionMode::kMaintenance), "OTA cannot bypass pairing teardown");
        const auto next = coordinator.Begin(target);
        Check(next && next.interrupted_purpose == AudioSessionMode::kDevicePairing, "higher priority owner requests pairing teardown");
        Check(!coordinator.OwnsDevicePairingResources(pairing.generation), "preemption immediately revokes pairing admission");
        Check(!coordinator.ReleaseDevicePairing(pairing.generation), "late pairing release never drops the new reservation");
        Check(coordinator.Complete(next), "higher priority operation completes only after caller's actual teardown");
    }
    for (bool fault : {false, true}) {
        AudioSessionCoordinator coordinator;
        const auto pairing = coordinator.Begin(AudioSessionMode::kDevicePairing);
        if (fault) Check(coordinator.Fail(pairing, false), "pairing preparation retains failed teardown");
        const auto recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
        Check(recording && recording.interrupted_purpose == AudioSessionMode::kDevicePairing,
            "recording can take responsibility for pending or failed pairing teardown");
        Check(!coordinator.ReleaseDevicePairing(pairing.generation), "old rollback cannot release recording");
    }
    AudioSessionCoordinator coordinator;
    const auto pairing = coordinator.Begin(AudioSessionMode::kDevicePairing);
    Check(!coordinator.ReleaseDevicePairing(0), "zero generation cannot release pairing");
    Check(coordinator.ReleaseDevicePairing(pairing.generation), "pairing acquisition rollback releases its exact lease");
    Check(!coordinator.ReleaseDevicePairing(pairing.generation), "pairing release is one-shot");
    const auto maintenance = coordinator.Begin(AudioSessionMode::kMaintenance);
    Check(coordinator.Complete(maintenance) && !coordinator.Begin(AudioSessionMode::kLocalRecording),
        "real firmware maintenance remains non-preemptible");
}

void TestFirmwareCommitRace() {
    for (int iteration = 0; iteration < 100; ++iteration) {
        AudioSessionCoordinator coordinator;
        const auto update = coordinator.Begin(AudioSessionMode::kFirmwareUpdate);
        Check(!coordinator.BeginFirmwareCommit(update.generation), "preparation cannot commit");
        Check(coordinator.Complete(update), "update reserve");
        Check(!coordinator.Begin(AudioSessionMode::kIdle), "update requires explicit release");
        std::atomic<bool> go{false};
        bool committed = false;
        AudioSessionTransition recording;
        std::thread commit([&] {
            while (!go.load()) std::this_thread::yield();
            committed = coordinator.BeginFirmwareCommit(update.generation);
        });
        std::thread capture([&] {
            while (!go.load()) std::this_thread::yield();
            recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
        });
        go = true;
        commit.join();
        capture.join();
        Check(committed != static_cast<bool>(recording), "only recording or boot commit can win");
        if (committed) {
            Check(coordinator.ReleaseFirmwareUpdate(update.generation, true), "joined committed worker releases I/O");
            Check(coordinator.Mode() == AudioSessionMode::kFirmwareCommit, "boot commitment remains pinned");
            Check(!coordinator.Begin(AudioSessionMode::kIdle) && !coordinator.BeginFaultCleanup() &&
                !coordinator.RecoverToIdle(true), "generic idle and fault cleanup cannot bypass commitment");
            Check(!coordinator.Begin(AudioSessionMode::kFileTransfer) && coordinator.BlocksSleep(),
                "boot commitment blocks transfer and sleep");
        } else {
            Check(!coordinator.ReleaseFirmwareUpdate(update.generation, true), "old update cannot release recording");
            Check(!coordinator.BeginFirmwareCommit(update.generation), "preempted generation cannot commit");
            Check(coordinator.Complete(recording), "recording completes after actual teardown");
        }
    }
    for (bool fault : {false, true}) {
        AudioSessionCoordinator coordinator;
        const auto update = coordinator.Begin(AudioSessionMode::kFirmwareUpdate);
        if (fault) Check(!coordinator.ReleaseFirmwareUpdate(update.generation, false), "failed cleanup retains fault");
        const auto recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
        Check(recording && recording.interrupted_purpose == AudioSessionMode::kFirmwareUpdate,
            "recording can finish pending update teardown");
        Check(!coordinator.ReleaseFirmwareUpdate(update.generation, true), "old teardown retains new owner");
    }
}

void TestModelValidationOwnership() {
    for (int phase = 0; phase < 3; ++phase) {
        AudioSessionCoordinator coordinator;
        const auto validation = coordinator.Begin(AudioSessionMode::kModelValidation);
        Check(validation && coordinator.OwnsModelValidationPreparation(validation.generation), "model preparation reserves resources");
        Check(!coordinator.OwnsModelValidationResources(validation.generation), "preparation is not capture permission");
        if (phase == 1) Check(coordinator.Complete(validation), "model validation capture starts after preparation");
        if (phase == 2) Check(!coordinator.ReleaseModelValidation(validation.generation, false), "model cleanup failure holds resources");
        Check(coordinator.OwnsModelValidationResources(validation.generation) == (phase == 1), "only running validation may capture");
        Check(!coordinator.Begin(AudioSessionMode::kIdle) && coordinator.BlocksSleep(), "validation cannot bypass checked cleanup");
        Check(!coordinator.Begin(AudioSessionMode::kAssistantConnecting), "validation cannot start a cloud assistant");
        const auto recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
        Check(recording && recording.interrupted_purpose == AudioSessionMode::kModelValidation, "recording preempts every validation phase");
        Check(!coordinator.ReleaseModelValidation(validation.generation, true), "old validation cannot release recording");
        Check(!coordinator.OwnsModelValidationResources(validation.generation), "preemption revokes validation capture immediately");
        Check(coordinator.Complete(recording), "recording starts after caller drains validation");
    }
}

void TestAssistantCleanupPreemptionScope() {
    for (auto phase : {AudioSessionMode::kAssistantConnecting, AudioSessionMode::kAssistantListening,
                       AudioSessionMode::kAssistantThinking, AudioSessionMode::kAssistantSpeaking}) {
        AudioSessionCoordinator coordinator;
        const auto assistant = coordinator.Begin(AudioSessionMode::kAssistantConnecting);
        Check(coordinator.Complete(assistant) && coordinator.UpdateAssistant(phase, assistant.generation),
            "assistant phase ready for cleanup");
        const auto cleanup = coordinator.BeginAssistantCleanup(assistant.generation);
        Check(cleanup && !coordinator.Begin(AudioSessionMode::kFileTransfer) &&
            !coordinator.Begin(AudioSessionMode::kAssistantConnecting), "only recording can take over assistant cleanup");
        const auto recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
        Check(recording && recording.interrupted_purpose == phase, "recording retains interrupted assistant purpose");
        Check(!coordinator.Complete(cleanup) && !coordinator.Fail(cleanup, true), "obsolete cleanup cannot release recording");
        Check(coordinator.Complete(recording), "recording handoff completes");
        const auto stop = coordinator.Begin(AudioSessionMode::kIdle);
        Check(stop && !coordinator.Begin(AudioSessionMode::kLocalRecording),
            "assistant cleanup permission does not leak into recording finalization");
        Check(coordinator.Complete(stop), "recording finalization completes");
        const auto maintenance = coordinator.Begin(AudioSessionMode::kMaintenance);
        Check(coordinator.Fail(maintenance, false), "maintenance fault retains resources");
        const auto recovery = coordinator.BeginFaultCleanup();
        Check(recovery && !coordinator.Begin(AudioSessionMode::kLocalRecording),
            "generic fault recovery is not assistant cleanup");
    }
}

int main() {
    TestAssistantCleanupPreemptionScope();
    TestModelValidationOwnership();
    TestFirmwareCommitRace();
    TestDevicePairingPreemption();
    TestRecordingPreemptsAssistant();
    TestPauseRetainsLease();
    TestFailedDrainRetainsLease();
    TestStaleTransitionRejected();
    TestConcurrentReservation();
    TestMaintenanceAndTransfer();
    TestTransferReleaseChecksOwnerAndPurpose();
    TestRecordingPreemptsPendingTransfer();
    TestLocalPlaybackIsolationAndPause(); TestRecordingPreemptsEveryPlaybackPhase(); TestPlaybackCannotReleaseAnotherFault();
    std::cout << "15 audio session tests passed\n";
}
