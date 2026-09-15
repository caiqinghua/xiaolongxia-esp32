#pragma once

#include "watch_pairing_state.h"
#include "watch_notification_state.h"
#include <utility>

struct WatchPairingSnapshot {
    bool available = false;
    bool active = false;
    bool can_open = false;
    bool can_remove_binding = false;
    bool can_authorize_cloud = false;
    bool can_inspect_saved_peers = false;
    uint64_t forget_peer_generation = 0;
    watch_voice::PairingState state;
    uint64_t confirmation = 0;
    uint32_t passkey = 0;
    bool compare_number = false;
    bool network_required = false;
};

// Only controller state can report a durable binding. Notification consent
// never authorizes account ownership, even though both use the BLE host.
inline WatchPairingSnapshot BuildWatchPairingSnapshot(watch_voice::PairingState state,
    bool available, bool active, bool can_open, const WatchAncsClientSnapshot& peer) {
    using Phase = watch_voice::PairingPhase;
    if (!available) return {};
    WatchPairingSnapshot result;
    result.available = true;
    result.active = active && state.attempt;
    result.can_open = can_open && !active &&
        (state.phase == Phase::kClosed || state.phase == Phase::kFailed || state.phase == Phase::kCancelled) &&
        state.error != watch_voice::PairingError::kStorage &&
        state.error != watch_voice::PairingError::kAlreadyBound;
    if (state.control_only || !result.active || state.phase != Phase::kAwaitApproval ||
        state.offer.device_id != state.device_id) state.offer = {};
    const bool expected_peer = state.control_only ? peer.control_pairing_peer : peer.device_pairing_peer;
    if (result.active && state.phase == Phase::kAwaitPeer && expected_peer &&
        peer.confirmation && (peer.state == WatchAncsLinkState::kPeerConsent ||
            (peer.state == WatchAncsLinkState::kConfirmNumber && peer.passkey <= 999999))) {
        result.confirmation = peer.confirmation;
        result.compare_number = peer.state == WatchAncsLinkState::kConfirmNumber;
        result.passkey = result.compare_number ? peer.passkey : 0;
    }
    result.state = std::move(state);
    return result;
}
