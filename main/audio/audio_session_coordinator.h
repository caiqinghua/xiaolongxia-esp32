#pragma once

#include <cstdint>
#include <mutex>

enum class AudioSessionMode {
    kIdle,
    kAssistantConnecting,
    kAssistantListening,
    kAssistantThinking,
    kAssistantSpeaking,
    kTransitioning,
    kLocalRecording,
    kRecordingPaused,
    kFileTransfer,
    kMaintenance,
    kSleeping,
    kFault,
    kLocalPlayback,
    kPlaybackPaused,
    kDevicePairing,
    kFirmwareUpdate,
    kFirmwareCommit,
    kModelValidation,
};

struct AudioSessionTransition {
    uint64_t generation = 0;
    AudioSessionMode previous = AudioSessionMode::kIdle;
    AudioSessionMode target = AudioSessionMode::kIdle;
    AudioSessionMode interrupted_purpose = AudioSessionMode::kIdle;
    explicit operator bool() const { return generation != 0; }
};

// Reserve before touching hardware, complete only after the side effects finish.
// A failed teardown retains the resource lease until explicit recovery succeeds.
class AudioSessionCoordinator {
public:
    AudioSessionTransition Begin(AudioSessionMode target);
    AudioSessionTransition BeginFaultCleanup(uint64_t expected_generation = 0);
    AudioSessionTransition BeginAssistantCleanup(uint64_t generation);
    bool Complete(const AudioSessionTransition& transition);
    bool Fail(const AudioSessionTransition& transition, bool resources_released);
    bool UpdateAssistant(AudioSessionMode mode, uint64_t generation);
    uint64_t RotateAssistant(uint64_t generation);
    bool EndAssistant(uint64_t generation);
    bool ReleaseFileTransfer(uint64_t generation);
    bool OwnsDevicePairingResources(uint64_t generation) const;
    bool ReleaseDevicePairing(uint64_t generation);
    bool OwnsFirmwareUpdateResources(uint64_t generation) const;
    // Pin the boot selection boundary before touching the boot partition table.
    bool BeginFirmwareCommit(uint64_t generation);
    // Call after joining the worker. A committed lease remains pinned until reboot.
    bool ReleaseFirmwareUpdate(uint64_t generation, bool resources_released);
    bool OwnsModelValidationPreparation(uint64_t generation) const;
    bool OwnsModelValidationResources(uint64_t generation) const;
    bool ReleaseModelValidation(uint64_t generation, bool resources_released);
    bool SetLocalPlaybackPaused(uint64_t generation, bool paused);
    // Call only after the file reader, decoder and output have actually released their resources.
    bool ReleaseLocalPlayback(uint64_t generation);
    bool OwnsLocalPlaybackResources(uint64_t generation) const;
    bool OwnsLocalPlaybackPreparation(uint64_t generation) const;
    bool MarkLocalPlaybackFault(uint64_t generation);
    static bool IsLocalPlayback(AudioSessionMode mode);
    bool RecoverToIdle(bool resources_released);

    AudioSessionMode Mode() const;
    uint64_t Generation() const;
    bool IsCurrent(uint64_t generation) const;
    bool AllowsAssistantResources() const;
    bool OwnsAssistantResources(uint64_t generation) const;
    bool OwnsAssistantSpeaking(uint64_t generation) const;
    bool OwnsAssistantListening(uint64_t generation) const;
    bool HoldsExclusiveResources(uint64_t generation) const;
    bool OwnsMaintenanceResources() const;
    bool BlocksSleep() const;
    static bool IsAssistant(AudioSessionMode mode);

private:
    static bool CanTransition(AudioSessionMode from, AudioSessionMode to);
    bool MatchesLocked(const AudioSessionTransition& transition) const;

    mutable std::mutex mutex_;
    AudioSessionMode mode_ = AudioSessionMode::kIdle;
    AudioSessionMode target_ = AudioSessionMode::kIdle;
    uint64_t generation_ = 1;
};
