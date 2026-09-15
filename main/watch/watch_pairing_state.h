#pragma once

#include <cstdint>
#include <string>
#include <array>
#include <vector>

namespace watch_voice {

struct PairingOffer {
    uint64_t request = 0;
    std::string device_id;
    std::string user_id;
    std::string challenge_id;
};

enum class PairingPhase { kClosed, kPreparing, kAwaitPeer, kReadingChallenge, kAwaitApproval,
                          kSigning, kAwaitCredential, kVerifying, kBound, kFailed, kCancelled, kControlReady,
                          kInspectingBinding, kAwaitRemoval, kRemovingBinding, kBindingRemoved,
                          kAwaitControlRemoval, kRemovingControl, kControlRemoved,
                          kInspectingSavedPeers, kAwaitSavedPeer, kAwaitAllControlRemoval,
                          kRemovingAllControl, kAllControlRemoved };
struct SavedControlPeer {
    uint64_t selection = 0;
    std::array<uint8_t, 6> address{};
    bool operator==(const SavedControlPeer& other) const {
        return selection == other.selection && address == other.address;
    }
};
enum class PairingError { kNone, kUnavailable, kAlreadyBound, kIdentity, kStorage, kClock,
                          kInvalidChallenge, kRejected, kVerification, kConflict, kTimeout, kNoBinding, kControlRemoval };
struct PairingState {
    PairingPhase phase = PairingPhase::kClosed;
    PairingError error = PairingError::kNone;
    std::string device_id;
    PairingOffer offer;
    uint64_t binding_generation = 0;
    uint64_t attempt = 0;
    bool control_only = false;
    bool removing_binding = false;
    bool removing_control = false;
    bool removing_saved_control = false;
    bool removing_all_control = false;
    std::vector<SavedControlPeer> saved_peers;
    uint64_t control_peer_generation = 0;
};

}  // namespace watch_voice
