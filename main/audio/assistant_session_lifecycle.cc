#include "assistant_session_lifecycle.h"

#include <algorithm>
#include <chrono>
#include <utility>

AssistantSessionLifecycle::AssistantSessionLifecycle(AudioSessionCoordinator& coordinator,
    Release close_transport, Release quiesce_audio, Resume resume_audio)
    : coordinator_(coordinator), close_transport_(std::move(close_transport)),
      quiesce_audio_(std::move(quiesce_audio)), resume_audio_(std::move(resume_audio)) {}

uint64_t AssistantSessionLifecycle::Begin() {
    RetireIfPreempted();
    if (token_ || !close_transport_ || !quiesce_audio_ || !resume_audio_) return 0;
    const auto reservation = coordinator_.Begin(AudioSessionMode::kAssistantConnecting);
    if (!reservation || !coordinator_.Complete(reservation)) return 0;
    token_ = reservation.generation;
    if (!resume_audio_() || !IsActive(token_)) {
        Stop(token_);
        return 0;
    }
    return token_;
}

bool AssistantSessionLifecycle::IsActive(uint64_t token) const {
    return token != 0 && token == token_ && !closing_ && !turning_ && coordinator_.OwnsAssistantResources(token);
}

bool AssistantSessionLifecycle::Update(AudioSessionMode mode, uint64_t token) {
    return token == token_ && !closing_ && !turning_ && coordinator_.UpdateAssistant(mode, token);
}

bool AssistantSessionLifecycle::BeginNextTurn(uint64_t token) {
    if (!IsActive(token)) return false;
    const auto next = coordinator_.RotateAssistant(token);
    if (!next) return false;
    token_ = next;
    turning_ = true;
    return true;
}

bool AssistantSessionLifecycle::PollNextTurn(uint32_t timeout_ms) {
    if (RetireIfPreempted() || !turning_) return false;
    if (!quiesce_audio_(timeout_ms)) return false;
    if (RetireIfPreempted()) return false;
    if (!resume_audio_()) {
        Stop(token_);
        return false;
    }
    if (RetireIfPreempted()) return false;
    turning_ = false;
    return IsActive(token_);
}

bool AssistantSessionLifecycle::RetireIfPreempted() {
    if (!token_ || IsActive(token_) || (turning_ && coordinator_.OwnsAssistantResources(token_)) ||
        (closing_ && coordinator_.IsCurrent(closing_.generation))) return false;
    // The new owner is responsible for cleanup. Never release its reservation.
    token_ = 0;
    closing_ = {};
    turning_ = false;
    return true;
}

bool AssistantSessionLifecycle::Stop(uint64_t token, uint32_t timeout_ms) {
    if (token == 0 || token != token_) return false;
    if (RetireIfPreempted()) return true;
    turning_ = false;
    if (!closing_) {
        closing_ = coordinator_.BeginAssistantCleanup(token);
        if (!closing_) return RetireIfPreempted();
    }
    return Poll(timeout_ms);
}

bool AssistantSessionLifecycle::Poll(uint32_t timeout_ms) {
    if (RetireIfPreempted()) return true;
    if (!closing_) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    if (!close_transport_(timeout_ms)) return false;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (!quiesce_audio_(static_cast<uint32_t>(std::max<int64_t>(0, remaining)))) return false;
    if (!coordinator_.Complete(closing_)) return RetireIfPreempted();
    token_ = 0;
    closing_ = {};
    resume_audio_();
    return true;
}
