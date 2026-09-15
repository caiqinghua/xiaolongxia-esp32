#pragma once

#include "audio_session_coordinator.h"
#include <functional>

// Called only by the application/protocol owner task. Competing recording and
// sleep owners synchronize through AudioSessionCoordinator, not this object.
class AssistantSessionLifecycle {
public:
    using Release = std::function<bool(uint32_t)>;
    using Resume = std::function<bool()>;
    AssistantSessionLifecycle(AudioSessionCoordinator& coordinator,
        Release close_transport, Release quiesce_audio, Resume resume_audio);

    uint64_t Begin();
    bool Update(AudioSessionMode mode, uint64_t token);
    bool Stop(uint64_t token, uint32_t timeout_ms = 0);
    bool Poll(uint32_t timeout_ms = 0);
    bool RetireIfPreempted();
    bool IsActive(uint64_t token) const;
    uint64_t Token() const { return token_; }
    bool IsClosing() const { return static_cast<bool>(closing_); }
    bool BeginNextTurn(uint64_t token);
    bool PollNextTurn(uint32_t timeout_ms = 0);
    bool IsTurning() const { return turning_; }

private:
    AudioSessionCoordinator& coordinator_;
    Release close_transport_;
    Release quiesce_audio_;
    Resume resume_audio_;
    uint64_t token_ = 0;
    AudioSessionTransition closing_;
    bool turning_ = false;
};
