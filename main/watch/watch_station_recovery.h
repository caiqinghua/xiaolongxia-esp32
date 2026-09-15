#pragma once

#include "audio/audio_session_coordinator.h"
#include <cstdint>
#include <functional>

enum class WatchStationRecoveryResult { kIdle, kDeferred, kCanceled, kStarted, kFailed };

// Owned by the application task. A successful attempt starts association; it
// does not claim that the station has obtained an IP address.
class WatchStationRecovery {
public:
    void Request();
    void Cancel();
    bool Pending() const { return pending_; }
    WatchStationRecoveryResult Poll(uint64_t now_ms, bool enabled, bool ready,
        AudioSessionCoordinator& coordinator, const std::function<bool()>& restore);

private:
    bool pending_ = false;
    unsigned attempts_ = 0;
    uint64_t last_attempt_ms_ = 0;
};
