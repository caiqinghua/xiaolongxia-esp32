#include "audio_session_coordinator.h"

bool AudioSessionCoordinator::IsAssistant(AudioSessionMode mode) {
    return mode == AudioSessionMode::kAssistantConnecting ||
        mode == AudioSessionMode::kAssistantListening ||
        mode == AudioSessionMode::kAssistantThinking ||
        mode == AudioSessionMode::kAssistantSpeaking;
}

bool AudioSessionCoordinator::IsLocalPlayback(AudioSessionMode mode) {
    return mode == AudioSessionMode::kLocalPlayback || mode == AudioSessionMode::kPlaybackPaused;
}

bool AudioSessionCoordinator::CanTransition(AudioSessionMode from, AudioSessionMode to) {
    if (from == AudioSessionMode::kTransitioning || from == AudioSessionMode::kFault ||
        from == AudioSessionMode::kSleeping || from == AudioSessionMode::kFirmwareCommit || from == to) {
        return false;
    }
    if (to == AudioSessionMode::kIdle) {
        return from != AudioSessionMode::kFirmwareUpdate && from != AudioSessionMode::kModelValidation;
    }
    if (from == AudioSessionMode::kMaintenance) {
        return false;
    }
    if (to == AudioSessionMode::kRecordingPaused) {
        return from == AudioSessionMode::kLocalRecording;
    }
    if (to == AudioSessionMode::kLocalRecording) {
        return from == AudioSessionMode::kIdle || IsAssistant(from) ||
            from == AudioSessionMode::kDevicePairing ||
            from == AudioSessionMode::kFirmwareUpdate ||
            from == AudioSessionMode::kModelValidation ||
            from == AudioSessionMode::kRecordingPaused || from == AudioSessionMode::kFileTransfer || IsLocalPlayback(from);
    }
    if (to == AudioSessionMode::kFileTransfer) {
        return from == AudioSessionMode::kIdle || IsAssistant(from) || from == AudioSessionMode::kDevicePairing ||
            from == AudioSessionMode::kFirmwareUpdate || from == AudioSessionMode::kModelValidation;
    }
    if (to == AudioSessionMode::kDevicePairing) return from == AudioSessionMode::kIdle;
    if (to == AudioSessionMode::kFirmwareUpdate) return from == AudioSessionMode::kIdle;
    if (to == AudioSessionMode::kModelValidation) return from == AudioSessionMode::kIdle;
    if (to == AudioSessionMode::kAssistantConnecting || to == AudioSessionMode::kMaintenance ||
        to == AudioSessionMode::kSleeping || to == AudioSessionMode::kLocalPlayback) {
        return from == AudioSessionMode::kIdle ||
            (to == AudioSessionMode::kSleeping && from == AudioSessionMode::kDevicePairing);
    }
    return false;
}

AudioSessionTransition AudioSessionCoordinator::Begin(AudioSessionMode target) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Recording may take responsibility for finishing transfer or playback teardown.
    // Its handoff must still confirm transfer and audio release before capture.
    const bool preempt_stream = target == AudioSessionMode::kLocalRecording &&
        (target_ == AudioSessionMode::kFileTransfer || target_ == AudioSessionMode::kDevicePairing ||
         target_ == AudioSessionMode::kFirmwareUpdate || target_ == AudioSessionMode::kModelValidation || IsLocalPlayback(target_)) &&
        (mode_ == AudioSessionMode::kTransitioning || mode_ == AudioSessionMode::kFault);
    const bool preempt_assistant_cleanup = target == AudioSessionMode::kLocalRecording &&
        mode_ == AudioSessionMode::kTransitioning && target_ == AudioSessionMode::kIdle &&
        IsAssistant(transition_source_);
    if (!preempt_stream && !preempt_assistant_cleanup && !CanTransition(mode_, target)) {
        return {};
    }
    const auto previous = mode_;
    const auto purpose = preempt_assistant_cleanup ? transition_source_ :
        ((mode_ == AudioSessionMode::kTransitioning || mode_ == AudioSessionMode::kFault) ? target_ : mode_);
    transition_source_ = previous;
    mode_ = AudioSessionMode::kTransitioning;
    target_ = target;
    ++generation_;
    if (generation_ == 0) {
        ++generation_;
    }
    return {generation_, previous, target, purpose};
}

bool AudioSessionCoordinator::MatchesLocked(const AudioSessionTransition& transition) const {
    return transition && mode_ == AudioSessionMode::kTransitioning &&
        transition.generation == generation_ && transition.target == target_;
}

AudioSessionTransition AudioSessionCoordinator::BeginFaultCleanup(uint64_t expected_generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_ != AudioSessionMode::kFault ||
        (expected_generation != 0 && expected_generation != generation_)) {
        return {};
    }
    transition_source_ = mode_;
    mode_ = AudioSessionMode::kTransitioning;
    target_ = AudioSessionMode::kIdle;
    ++generation_;
    return {generation_, AudioSessionMode::kFault, target_};
}

bool AudioSessionCoordinator::Complete(const AudioSessionTransition& transition) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!MatchesLocked(transition)) {
        return false;
    }
    mode_ = target_;
    return true;
}

AudioSessionTransition AudioSessionCoordinator::BeginAssistantCleanup(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == 0 || generation != generation_ || !IsAssistant(mode_)) return {};
    const auto previous = mode_;
    transition_source_ = previous;
    mode_ = AudioSessionMode::kTransitioning;
    target_ = AudioSessionMode::kIdle;
    if (++generation_ == 0) ++generation_;
    return {generation_, previous, target_};
}

bool AudioSessionCoordinator::Fail(const AudioSessionTransition& transition, bool resources_released) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!MatchesLocked(transition)) {
        return false;
    }
    mode_ = resources_released ? AudioSessionMode::kIdle : AudioSessionMode::kFault;
    return true;
}

bool AudioSessionCoordinator::UpdateAssistant(AudioSessionMode mode, uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation != generation_ || !IsAssistant(mode_) || !IsAssistant(mode)) {
        return false;
    }
    mode_ = mode;
    return true;
}

uint64_t AudioSessionCoordinator::RotateAssistant(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!generation || generation != generation_ || !IsAssistant(mode_)) return 0;
    if (++generation_ == 0) ++generation_;
    mode_ = target_ = AudioSessionMode::kAssistantConnecting;
    return generation_;
}

bool AudioSessionCoordinator::EndAssistant(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation != generation_ || !IsAssistant(mode_)) {
        return false;
    }
    mode_ = AudioSessionMode::kIdle;
    ++generation_;
    return true;
}

bool AudioSessionCoordinator::RecoverToIdle(bool resources_released) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!resources_released || (mode_ != AudioSessionMode::kFault && mode_ != AudioSessionMode::kSleeping)) {
        return false;
    }
    mode_ = AudioSessionMode::kIdle;
    ++generation_;
    return true;
}

bool AudioSessionCoordinator::ReleaseFileTransfer(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!generation || generation != generation_ ||
        (mode_ != AudioSessionMode::kFileTransfer &&
            !((mode_ == AudioSessionMode::kTransitioning || mode_ == AudioSessionMode::kFault) &&
              target_ == AudioSessionMode::kFileTransfer))) return false;
    mode_ = target_ = AudioSessionMode::kIdle;
    if (++generation_ == 0) ++generation_;
    return true;
}

bool AudioSessionCoordinator::OwnsDevicePairingResources(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation && generation == generation_ &&
        (mode_ == AudioSessionMode::kDevicePairing ||
         (mode_ == AudioSessionMode::kTransitioning && target_ == AudioSessionMode::kDevicePairing));
}

bool AudioSessionCoordinator::ReleaseDevicePairing(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!generation || generation != generation_ ||
        (mode_ != AudioSessionMode::kDevicePairing &&
         !((mode_ == AudioSessionMode::kTransitioning || mode_ == AudioSessionMode::kFault) &&
           target_ == AudioSessionMode::kDevicePairing))) return false;
    mode_ = target_ = AudioSessionMode::kIdle;
    if (++generation_ == 0) ++generation_;
    return true;
}

bool AudioSessionCoordinator::OwnsFirmwareUpdateResources(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation && generation == generation_ &&
        (mode_ == AudioSessionMode::kFirmwareUpdate || mode_ == AudioSessionMode::kFirmwareCommit ||
         (mode_ == AudioSessionMode::kTransitioning && target_ == AudioSessionMode::kFirmwareUpdate));
}

bool AudioSessionCoordinator::BeginFirmwareCommit(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!generation || generation != generation_ || mode_ != AudioSessionMode::kFirmwareUpdate) return false;
    mode_ = target_ = AudioSessionMode::kFirmwareCommit;
    return true;
}

bool AudioSessionCoordinator::ReleaseFirmwareUpdate(uint64_t generation, bool resources_released) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!generation || generation != generation_) return false;
    // Even an uncertain boot selection must not enter generic fault recovery.
    if (mode_ == AudioSessionMode::kFirmwareCommit) return resources_released;
    if (mode_ != AudioSessionMode::kFirmwareUpdate &&
        !((mode_ == AudioSessionMode::kTransitioning || mode_ == AudioSessionMode::kFault) &&
          target_ == AudioSessionMode::kFirmwareUpdate)) return false;
    if (!resources_released) {
        mode_ = AudioSessionMode::kFault;
        target_ = AudioSessionMode::kFirmwareUpdate;
        return false;
    }
    mode_ = target_ = AudioSessionMode::kIdle;
    if (++generation_ == 0) ++generation_;
    return true;
}

bool AudioSessionCoordinator::OwnsModelValidationPreparation(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation && generation == generation_ && mode_ == AudioSessionMode::kTransitioning &&
        target_ == AudioSessionMode::kModelValidation;
}

bool AudioSessionCoordinator::OwnsModelValidationResources(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation && generation == generation_ && mode_ == AudioSessionMode::kModelValidation;
}

bool AudioSessionCoordinator::ReleaseModelValidation(uint64_t generation, bool resources_released) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!generation || generation != generation_ || (mode_ != AudioSessionMode::kModelValidation &&
        !((mode_ == AudioSessionMode::kTransitioning || mode_ == AudioSessionMode::kFault) &&
          target_ == AudioSessionMode::kModelValidation))) return false;
    mode_ = resources_released ? AudioSessionMode::kIdle : AudioSessionMode::kFault;
    target_ = resources_released ? AudioSessionMode::kIdle : AudioSessionMode::kModelValidation;
    if (resources_released && ++generation_ == 0) ++generation_;
    return resources_released;
}

bool AudioSessionCoordinator::SetLocalPlaybackPaused(uint64_t generation, bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!generation || generation != generation_ || !IsLocalPlayback(mode_)) return false;
    mode_ = target_ = paused ? AudioSessionMode::kPlaybackPaused : AudioSessionMode::kLocalPlayback;
    return true;
}

bool AudioSessionCoordinator::OwnsLocalPlaybackResources(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation && generation == generation_ && IsLocalPlayback(mode_);
}

bool AudioSessionCoordinator::OwnsLocalPlaybackPreparation(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation && generation == generation_ && mode_ == AudioSessionMode::kTransitioning &&
        target_ == AudioSessionMode::kLocalPlayback;
}

bool AudioSessionCoordinator::ReleaseLocalPlayback(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!generation || generation != generation_ || (!IsLocalPlayback(mode_) &&
        !((mode_ == AudioSessionMode::kTransitioning || mode_ == AudioSessionMode::kFault) && IsLocalPlayback(target_))))
        return false;
    mode_ = target_ = AudioSessionMode::kIdle;
    if (++generation_ == 0) ++generation_;
    return true;
}

bool AudioSessionCoordinator::MarkLocalPlaybackFault(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!generation || generation != generation_ || (!IsLocalPlayback(mode_) &&
        !((mode_ == AudioSessionMode::kTransitioning || mode_ == AudioSessionMode::kFault) && IsLocalPlayback(target_))))
        return false;
    mode_ = AudioSessionMode::kFault;
    target_ = AudioSessionMode::kLocalPlayback;
    return true;
}

AudioSessionMode AudioSessionCoordinator::Mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mode_;
}

uint64_t AudioSessionCoordinator::Generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

bool AudioSessionCoordinator::IsCurrent(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation != 0 && generation == generation_;
}

bool AudioSessionCoordinator::AllowsAssistantResources() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mode_ == AudioSessionMode::kIdle || IsAssistant(mode_);
}

bool AudioSessionCoordinator::OwnsAssistantResources(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation != 0 && generation == generation_ && IsAssistant(mode_);
}

bool AudioSessionCoordinator::OwnsAssistantSpeaking(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation != 0 && generation == generation_ && mode_ == AudioSessionMode::kAssistantSpeaking;
}

bool AudioSessionCoordinator::OwnsAssistantListening(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation != 0 && generation == generation_ && mode_ == AudioSessionMode::kAssistantListening;
}

bool AudioSessionCoordinator::HoldsExclusiveResources(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation != 0 && generation == generation_ &&
        mode_ != AudioSessionMode::kIdle && !IsAssistant(mode_);
}

bool AudioSessionCoordinator::BlocksSleep() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mode_ != AudioSessionMode::kIdle;
}

bool AudioSessionCoordinator::OwnsMaintenanceResources() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (mode_ == AudioSessionMode::kTransitioning && target_ == AudioSessionMode::kMaintenance) ||
        mode_ == AudioSessionMode::kMaintenance;
}
