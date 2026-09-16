#include "application.h"
#include "board.h"
#include "blufi.h"
#include "wifi_manager.h"
#include "watch/watch_ancs_client.h"
#include "watch/watch_view_state.h"
#include "watch/watch_wifi_gate.h"
#include "system/audream_power_manager.h"
#include "system/device_time_service.h"
#include <algorithm>

void Application::InitializeWatchPairing() {
    if (watch_pairing_) return;
    watch_voice::PairingController::Hooks hooks;
    hooks.acquire = [this] { return AcquireWatchPairing(); };
    hooks.release = [this] { ReleaseWatchPairing(); };
    hooks.eligible = [this] {
        if (watch_pairing_revoked_.load() || GetWatchWifiGate().TransferActive() ||
            ((watch_pairing_removal_.load() || watch_pairing_control_removal_.load() || watch_pairing_saved_removal_.load()) && audio_service_.IsAssistantUserEnabled()) ||
            (!watch_pairing_control_only_.load() && !watch_pairing_removal_.load() && !watch_pairing_control_removal_.load() && !watch_pairing_saved_removal_.load() &&
             (!GetWatchWifiGate().Enabled() || !GetWatchWifiGate().StationAllowed()))) return false;
        const auto generation = watch_pairing_audio_generation_.load();
        return generation ? audio_session_coordinator_.OwnsDevicePairingResources(generation)
                          : audio_session_coordinator_.Mode() == AudioSessionMode::kIdle;
    };
    hooks.unix_seconds = [] {
        auto& clock = DeviceTimeService::GetInstance();
        return clock.IsSynchronized() ? clock.CurrentUnixTimeMs() / 1000 : 0;
    };
    hooks.prepare_tls_clock = [] { return PrepareWatchTlsClock(); };
    watch_pairing_ = std::make_unique<watch_voice::PairingController>(WatchVoiceEndpoints(), std::move(hooks));
}

bool Application::CanOpenWatchPairing(bool control_only, bool removing_binding, uint64_t control_peer, bool saved_control) {
    auto& gate = GetWatchWifiGate();
    return watch_pairing_ && !watch_pairing_->Active() && !gate.TransferActive() &&
        (!(removing_binding || control_peer || saved_control) || !audio_service_.IsAssistantUserEnabled()) &&
        (control_only || removing_binding || control_peer || saved_control || (WatchVoiceEndpoints().Valid() && gate.Enabled() && gate.StationAllowed() &&
            WifiManager::GetInstance().IsConnected())) &&
        (removing_binding || (Blufi::GetInstance().IsControlReady() &&
            (saved_control ? WatchAncsClient::Get().CanInspectSavedControlPeers() : control_peer ? WatchAncsClient::Get().CanForgetControlPeer(control_peer) :
             WatchAncsClient::Get().CanOpenPairingWindow(control_only)))) &&
        !Board::GetInstance().IsWatchWifiProvisioning() &&
        !Board::GetInstance().GetWatchRecordingControlStatus().starting &&
        audio_session_coordinator_.Mode() == AudioSessionMode::kIdle && audio_service_.IsIdle() &&
        audio_service_.IsAssistantCaptureDrained() && !audio_service_.IsWakeWordRunning() &&
        !HasAssistantSession() && !assistant_session_.IsClosing() && !pending_assistant_listening_ &&
        !reset_protocol_pending_ && !watch_cloud_task_.Active() && !watch_cloud_application_ &&
        !watch_cloud_retirement_ && !watch_wake_retirement_ && !watch_cloud_ack_ && !watch_power_change_ &&
        !watch_network_preparing_ && !watch_deferred_wifi_request_ && !watch_library_request_;
}

bool Application::AcquireWatchPairing() {
    // No driver starts or asynchronous teardown occur in acquire: a false return
    // must leave no resource retained outside the controller's reservation.
    const bool removing = watch_pairing_removal_.load();
    const auto control_peer = watch_pairing_control_removal_.load();
    const bool saved = watch_pairing_saved_removal_.load();
    if (!CanOpenWatchPairing(watch_pairing_control_only_.load(), removing, control_peer, saved) || watch_pairing_revoked_.load()) return false;
    const auto reservation = audio_session_coordinator_.Begin(AudioSessionMode::kDevicePairing);
    if (!reservation) return false;
    watch_pairing_audio_generation_.store(reservation.generation);
    if (!removing) watch_pairing_ble_ = Blufi::GetInstance().AcquireControlLease();
    auto& power = AudreamPowerManager::GetInstance();
    if (removing || watch_pairing_ble_) watch_pairing_storage_ = power.BeginStorageRead();
    if (watch_pairing_storage_ && !removing && !control_peer && !saved) watch_pairing_network_ = power.BeginVoiceNetworkRequest();
    if (!watch_pairing_storage_ || (!removing && !control_peer && !saved && !watch_pairing_network_) || watch_pairing_revoked_.load() ||
        (!watch_pairing_control_only_.load() && !removing && !control_peer && !saved && !GetWatchWifiGate().Enabled()) ||
        (removing && protocol_ && !protocol_->CloseAudioChannelWithin(0)) ||
        !audio_session_coordinator_.Complete(reservation)) {
        ReleaseWatchPairing();
        return false;
    }
    watch_wake_lease_.Revoke();
    if (removing) {
        protocol_.reset();
        watch_cloud_authorization_.reset();
        watch_cloud_retry_after_us_ = 0;
        watch_wake_confirmed_revision_ = 0;
    }
    return true;
}

void Application::ReleaseWatchPairing() {
    watch_pairing_revoked_.store(true);
    const auto generation = watch_pairing_audio_generation_.exchange(0);
    if (watch_pairing_ble_) {
        Blufi::GetInstance().ReleaseControlLease(watch_pairing_ble_);
        watch_pairing_ble_ = 0;
    }
    auto& power = AudreamPowerManager::GetInstance();
    if (watch_pairing_network_) { power.EndVoiceNetworkRequest(); watch_pairing_network_ = false; }
    if (watch_pairing_storage_) { power.EndStorageRead(); watch_pairing_storage_ = false; }
    // A recording/transfer handoff owns its newer generation. Never reset it.
    if (generation) audio_session_coordinator_.ReleaseDevicePairing(generation);
}

void Application::ObserveWatchPairingIntent(const WatchViewRequest& request) {
    const bool cancel = request.action == WatchViewAction::kCancelDevicePairing;
    const bool wifi_off = request.action == WatchViewAction::kSetWifiEnabled && !request.enabled &&
        !watch_pairing_control_only_.load() && !watch_pairing_removal_.load() && !watch_pairing_control_removal_.load() && !watch_pairing_saved_removal_.load();
    const bool voice_on = request.action == WatchViewAction::kSetVoiceEnabled && request.enabled &&
        (watch_pairing_removal_.load() || watch_pairing_control_removal_.load() || watch_pairing_saved_removal_.load());
    const bool reject = !request.enabled &&
        (request.action == WatchViewAction::kConfirmDevicePairingPeer ||
         request.action == WatchViewAction::kApproveDevicePairingAccount ||
         request.action == WatchViewAction::kConfirmBindingRemoval || request.action == WatchViewAction::kConfirmControlRemoval);
    const bool reject_all = !request.enabled && request.action == WatchViewAction::kConfirmAllControlRemoval;
    if (!cancel && !wifi_off && !voice_on && !reject && !reject_all) return;
    std::lock_guard<std::mutex> lock(watch_pairing_intent_mutex_);
    if (cancel && request.pairing_open_request)
        watch_pairing_cancelled_open_ = std::max(watch_pairing_cancelled_open_, request.pairing_open_request);
    const bool same_open = cancel && request.pairing_open_request &&
        request.pairing_open_request == watch_pairing_open_request_;
    const bool same_attempt = request.pairing_attempt && request.pairing_attempt == watch_pairing_attempt_;
    const auto pairing_state = watch_pairing_ ? watch_pairing_->Snapshot() : watch_voice::PairingState{};
    const bool same_rejection = (reject || reject_all) && same_attempt &&
        ((request.action == WatchViewAction::kConfirmDevicePairingPeer && request.pairing_confirmation &&
          request.pairing_confirmation == watch_pairing_peer_confirmation_) ||
         (request.action == WatchViewAction::kApproveDevicePairingAccount && request.pairing_request &&
          request.pairing_request == watch_pairing_account_request_) ||
         (request.action == WatchViewAction::kConfirmBindingRemoval && request.binding_generation &&
          request.binding_generation == watch_pairing_account_request_) ||
         (request.action == WatchViewAction::kConfirmControlRemoval && request.pairing_peer_generation &&
          request.pairing_peer_generation == watch_pairing_account_request_) ||
         (request.action == WatchViewAction::kConfirmAllControlRemoval && pairing_state.removing_all_control));
    if (same_rejection) watch_pairing_rejected_request_ = request.id;
    if (watch_pairing_ && (wifi_off || voice_on || same_rejection ||
        (cancel && (request.pairing_open_request ? same_open : same_attempt)))) {
        watch_pairing_revoked_.store(true);
        watch_pairing_->Cancel();
    }
}

bool Application::HandleWatchPairingRequest(const WatchViewRequest& request) {
    if (!watch_pairing_ || !request.id) return false;
    const auto state = watch_pairing_->Snapshot();
    if (request.action == WatchViewAction::kOpenDevicePairing || request.action == WatchViewAction::kOpenBindingRemoval ||
        request.action == WatchViewAction::kOpenControlRemoval || request.action == WatchViewAction::kOpenSavedControlPeers) {
        const bool removing = request.action == WatchViewAction::kOpenBindingRemoval;
        const bool saved = request.action == WatchViewAction::kOpenSavedControlPeers;
        const auto control_peer = request.action == WatchViewAction::kOpenControlRemoval ? request.pairing_peer_generation : 0;
        if (request.action == WatchViewAction::kOpenControlRemoval && !control_peer) return false;
        if (request.pairing_attempt != state.attempt || !CanOpenWatchPairing(!saved && request.pairing_control_only, removing, control_peer, saved)) return false;
        {
            std::lock_guard<std::mutex> lock(watch_pairing_intent_mutex_);
            if (request.id <= watch_pairing_cancelled_open_) return false;
            watch_pairing_open_request_ = request.id;
            watch_pairing_peer_confirmation_ = watch_pairing_account_request_ = watch_pairing_rejected_request_ = 0;
            watch_pairing_revoked_.store(false);
            watch_pairing_control_only_.store(!removing && !control_peer && !saved && request.pairing_control_only);
            watch_pairing_removal_.store(removing);
            watch_pairing_saved_removal_.store(saved);
            watch_pairing_control_removal_.store(control_peer);
        }
        const bool accepted = saved ? watch_pairing_->OpenSavedControlPeers() : removing ? watch_pairing_->OpenBindingRemoval() : control_peer
            ? watch_pairing_->OpenControlRemoval(control_peer) : watch_pairing_->Open(request.pairing_control_only);
        {
            std::lock_guard<std::mutex> lock(watch_pairing_intent_mutex_);
            watch_pairing_attempt_ = watch_pairing_->Snapshot().attempt;
        }
        return accepted;
    }
    if (request.action == WatchViewAction::kCancelDevicePairing) {
        if (request.pairing_open_request) {
            std::lock_guard<std::mutex> lock(watch_pairing_intent_mutex_);
            if (request.pairing_open_request != watch_pairing_open_request_) return false;
        } else if (!request.pairing_attempt || request.pairing_attempt != state.attempt) return false;
        StopWatchPairing(0);
        return true;
    }
    if (!request.pairing_attempt || request.pairing_attempt != state.attempt) return false;
    if (request.action == WatchViewAction::kSelectSavedControlPeer)
        return watch_pairing_->SelectSavedControlPeer(request.pairing_peer_generation);
    if (request.action == WatchViewAction::kSelectAllSavedControlPeers)
        return watch_pairing_->SelectAllSavedControlPeers();
    bool rejected = false;
    {
        std::lock_guard<std::mutex> lock(watch_pairing_intent_mutex_);
        rejected = !request.enabled && request.id == watch_pairing_rejected_request_ &&
            (request.action == WatchViewAction::kConfirmDevicePairingPeer ||
             request.action == WatchViewAction::kApproveDevicePairingAccount ||
             request.action == WatchViewAction::kConfirmBindingRemoval || request.action == WatchViewAction::kConfirmControlRemoval ||
             request.action == WatchViewAction::kConfirmAllControlRemoval);
    }
    if (rejected) { StopWatchPairing(0); return true; }
    if (request.action == WatchViewAction::kConfirmDevicePairingPeer)
        return watch_pairing_->ConfirmPeer(request.pairing_confirmation, request.enabled);
    if (request.action == WatchViewAction::kApproveDevicePairingAccount)
        return watch_pairing_->ApproveAccount(request.pairing_request, request.enabled);
    if (request.action == WatchViewAction::kConfirmBindingRemoval)
        return watch_pairing_->ConfirmBindingRemoval(request.binding_generation, request.enabled);
    if (request.action == WatchViewAction::kConfirmControlRemoval)
        return watch_pairing_->ConfirmControlRemoval(request.pairing_peer_generation, request.enabled);
    if (request.action == WatchViewAction::kConfirmAllControlRemoval)
        return watch_pairing_->ConfirmAllControlRemoval(request.enabled);
    return false;
}

bool Application::StopWatchPairing(uint32_t timeout_ms) {
    watch_pairing_revoked_.store(true);
    return !watch_pairing_ || watch_pairing_->StopWithin(timeout_ms);
}

WatchPairingSnapshot Application::PollWatchPairing() {
    if (!watch_pairing_) return {};
    watch_pairing_->Poll();
    auto raw = watch_pairing_->Snapshot();
    const auto peer = WatchAncsClient::Get().Snapshot();
    if (raw.control_only && raw.phase == watch_voice::PairingPhase::kControlReady &&
        (peer.state != WatchAncsLinkState::kControlReady || peer.link_generation != raw.control_peer_generation)) {
        raw.phase = watch_voice::PairingPhase::kClosed;
        raw.control_peer_generation = 0;
    }
    {
        std::lock_guard<std::mutex> lock(watch_pairing_intent_mutex_);
        watch_pairing_account_request_ = raw.removing_binding && raw.phase == watch_voice::PairingPhase::kAwaitRemoval
            ? raw.binding_generation : raw.removing_control && raw.phase == watch_voice::PairingPhase::kAwaitControlRemoval
            ? raw.control_peer_generation : raw.offer.request;
        const bool expected_peer = raw.control_only ? peer.control_pairing_peer : peer.device_pairing_peer;
        watch_pairing_peer_confirmation_ = raw.phase == watch_voice::PairingPhase::kAwaitPeer && expected_peer
            ? peer.confirmation : 0;
    }
    auto state = BuildWatchPairingSnapshot(raw, true,
        watch_pairing_->Active(), CanOpenWatchPairing(true), peer);
    // Cloud binding errors do not prevent an independent local control pairing.
    if (!watch_pairing_->Active() && raw.phase != watch_voice::PairingPhase::kControlReady)
        state.can_open = CanOpenWatchPairing(true);
    state.can_remove_binding = CanOpenWatchPairing(false, true) && raw.error != watch_voice::PairingError::kStorage;
    state.can_authorize_cloud = CanOpenWatchPairing(false) && raw.error != watch_voice::PairingError::kStorage;
    state.can_inspect_saved_peers = CanOpenWatchPairing(false, false, 0, true);
    state.forget_peer_generation = CanOpenWatchPairing(false, false, peer.link_generation) &&
        WatchAncsClient::Get().CanForgetControlPeer(peer.link_generation) ? peer.link_generation : 0;
    state.network_required = watch_pairing_->Active() && !raw.control_only && !raw.removing_binding && !raw.removing_control &&
        (!GetWatchWifiGate().Enabled() || !GetWatchWifiGate().StationAllowed() || !WifiManager::GetInstance().IsConnected());
    return state;
}
