#pragma once

#include "watch_voice_identity.h"
#include "watch_pairing_state.h"
#include <mutex>

namespace watch_voice {

// Owned by the watch's physical confirmation flow, not a remote BLE command.
// Secure peer generations must change at disconnect/security loss/reconnect.
class PairingSession {
public:
    bool OpenLocalWindow(uint64_t now_ms);
    void Cancel();
    PairingOffer Offer(uint64_t secure_peer_generation, std::string_view payload,
                       uint64_t unix_seconds, uint64_t now_ms);
    bool ApproveLocally(uint64_t request, uint64_t now_ms);
    bool TakeProof(uint64_t request, uint64_t secure_peer_generation, uint64_t now_ms,
                   std::array<uint8_t, 64>& result);
    void Disconnect(uint64_t secure_peer_generation);

private:
    void Clear();
    std::mutex mutex_;
    uint64_t sequence_ = 0, request_ = 0, peer_ = 0, deadline_ms_ = 0, window_ms_ = 0, last_ms_ = 0;
    bool approved_ = false;
    DeviceIdentity identity_;
    std::string payload_;
    bool ValidTime(uint64_t now_ms);
};

}  // namespace watch_voice
