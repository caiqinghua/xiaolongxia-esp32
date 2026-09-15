#include "application.h"
#include "board.h"
#include "blufi.h"
#include "system/audream_power_manager.h"
#include "system/device_time_service.h"
#include "watch/watch_wifi_gate.h"
#include "protocols/assistant_transport_handoff.h"
#include <deque>

// Reuse the same native I/O fixtures, real signer/HTTP/NVS and transport boundary.
// Only the entry point changes: requests now go through Application's methods.
#define main PairingControllerFixtureMain
#include "watch_pairing_controller_test.cc"
#undef main

watch_voice::Endpoints Application::WatchVoiceEndpoints() { return endpoints; }
bool Application::PrepareWatchTlsClock() {
    assert(std::this_thread::get_id() != owner && HasReservation());
    return tls_ok;
}
WatchAncsClientSnapshot WatchAncsClient::Snapshot() const {
    std::lock_guard<std::mutex> lock(transport_mutex);
    WatchAncsClientSnapshot snapshot;
    snapshot.device_pairing_peer = live_window != 0 && !local_control_mode;
    snapshot.control_pairing_peer = live_window != 0 && local_control_mode;
    snapshot.confirmation = peer_confirmed ? 0 : 1;
    snapshot.state = peer_confirmed ? WatchAncsLinkState::kDevicePairingReady : WatchAncsLinkState::kPeerConsent;
    snapshot.link_generation = lease;
    if (local_control_grant) snapshot.state = WatchAncsLinkState::kControlReady;
    return snapshot;
}

namespace {
uint64_t next_ui = 0;
WatchViewRequest Request(Application& app, WatchViewAction action, bool enabled = false) {
    WatchViewRequest request;
    request.id = ++next_ui;
    request.action = action;
    request.enabled = enabled;
    request.pairing_attempt = app.watch_pairing_->Snapshot().attempt;
    return request;
}
void ReachApp(Application& app, Phase phase) {
    Wait([&] { return app.PollWatchPairing().state.phase == phase; });
}
WatchViewRequest OpenApp(Application& app) {
    auto request = Request(app, WatchViewAction::kOpenDevicePairing);
    app.ObserveWatchPairingIntent(request);
    assert(app.HandleWatchPairingRequest(request));
    ReachApp(app, Phase::kAwaitPeer);
    assert(HasReservation() && app.audio_session_coordinator_.Mode() == AudioSessionMode::kDevicePairing);
    return request;
}
WatchViewRequest OfferApp(Application& app) {
    OpenApp(app);
    auto peer = Request(app, WatchViewAction::kConfirmDevicePairingPeer, true);
    peer.pairing_confirmation = app.PollWatchPairing().confirmation;
    assert(peer.pairing_confirmation && app.HandleWatchPairingRequest(peer));
    Send(Transport::Kind::kChallenge, Payload()); ReachApp(app, Phase::kAwaitApproval);
    auto approval = Request(app, WatchViewAction::kApproveDevicePairingAccount, true);
    approval.pairing_request = app.PollWatchPairing().state.offer.request;
    return approval;
}
void VerifyApp(Application& app) {
    auto approval = OfferApp(app);
    assert(app.HandleWatchPairingRequest(approval)); ReachApp(app, Phase::kAwaitCredential);
    Send(Transport::Kind::kCredential, credential); app.PollWatchPairing();
    assert(app.watch_pairing_->Snapshot().phase == Phase::kVerifying);
}
}

int main(int argc, char** argv) {
    assert(argc == 2); Initialize();
    const std::string scenario = argv[1];
    GetWatchWifiGate().Initialize(true);
    Application app;
    app.InitializeWatchPairing();
    auto& power = AudreamPowerManager::GetInstance();
    reservation_probe = [&] {
        if (app.watch_pairing_control_removal_.load() || app.watch_pairing_saved_removal_.load())
            return power.storage_leases == 1 && power.network_leases == 0 && Blufi::GetInstance().lease;
        return power.storage_leases == 1 && (app.watch_pairing_removal_.load()
            ? power.network_leases == 0 && !Blufi::GetInstance().lease
            : power.network_leases == 1 && Blufi::GetInstance().lease);
    };
    if (scenario.rfind("saved-", 0) == 0) {
        saved_allowed = true;
        saved_remaining = 2;
        saved_forget_fail_call = scenario == "saved-all-second-failed" ? 2 : 0;
        app.audio_service_.enabled = scenario == "saved-enabled";
        WifiStation::GetInstance().connected = false;
        GetWatchWifiGate().Initialize(false);
        if (scenario == "saved-busy") app.watch_library_request_ = 1;
        if (scenario == "saved-native-busy") saved_allowed = false;
        if (scenario == "saved-storage-lease") power.allowed = false;
        if (scenario == "saved-ble-lease") Blufi::GetInstance().lease_allowed = false;
        auto open = Request(app, WatchViewAction::kOpenSavedControlPeers);
        if (scenario == "saved-queued-cancel") {
            auto cancel = Request(app, WatchViewAction::kCancelDevicePairing);
            cancel.pairing_open_request = open.id;
            app.ObserveWatchPairingIntent(cancel);
        }
        const bool blocked = scenario == "saved-enabled" || scenario == "saved-busy" || scenario == "saved-native-busy" ||
            scenario == "saved-storage-lease" || scenario == "saved-ble-lease" || scenario == "saved-queued-cancel";
        if (blocked) assert(!app.HandleWatchPairingRequest(open));
        else {
            assert(app.PollWatchPairing().can_inspect_saved_peers);
            assert(app.HandleWatchPairingRequest(open));
            ReachApp(app, Phase::kAwaitSavedPeer);
            assert(HasReservation() && !app.PollWatchPairing().network_required);
            if (scenario.rfind("saved-all-", 0) == 0) {
                auto select_all = Request(app, WatchViewAction::kSelectAllSavedControlPeers);
                --select_all.pairing_attempt; assert(!app.HandleWatchPairingRequest(select_all)); ++select_all.pairing_attempt;
                assert(app.HandleWatchPairingRequest(select_all));
                assert(app.PollWatchPairing().state.phase == Phase::kAwaitAllControlRemoval);
                auto confirm_all = Request(app, WatchViewAction::kConfirmAllControlRemoval,
                    scenario != "saved-all-reject");
                app.ObserveWatchPairingIntent(confirm_all);
                assert(app.HandleWatchPairingRequest(confirm_all));
                ReachApp(app, scenario == "saved-all-reject" ? Phase::kCancelled :
                    scenario == "saved-all-second-failed" ? Phase::kFailed : Phase::kAllControlRemoved);
                assert(forget_calls == (scenario == "saved-all-reject" ? 0U :
                    scenario == "saved-all-second-failed" ? 1U : 2U));
            } else {
            auto select = Request(app, WatchViewAction::kSelectSavedControlPeer);
            select.pairing_peer_generation = lease + 1;
            --select.pairing_attempt; assert(!app.HandleWatchPairingRequest(select)); ++select.pairing_attempt;
            assert(app.HandleWatchPairingRequest(select));
            assert(app.PollWatchPairing().state.phase == Phase::kAwaitControlRemoval);
            auto wifi_off = Request(app, WatchViewAction::kSetWifiEnabled, false);
            app.ObserveWatchPairingIntent(wifi_off);
            assert(!app.watch_pairing_revoked_.load());
            auto confirm = Request(app, WatchViewAction::kConfirmControlRemoval, scenario != "saved-reject");
            confirm.pairing_peer_generation = lease + 1;
            if (scenario == "saved-enable-cancel") {
                auto enable = Request(app, WatchViewAction::kSetVoiceEnabled, true);
                app.ObserveWatchPairingIntent(enable);
                assert(!app.HandleWatchPairingRequest(confirm));
                ReachApp(app, Phase::kCancelled);
            } else {
                const bool gated = scenario == "saved-cancel-active" || scenario == "saved-recording-handoff";
                if (gated) forget_gate.Block();
                app.ObserveWatchPairingIntent(confirm);
                assert(app.HandleWatchPairingRequest(confirm));
                if (gated) {
                    forget_gate.Await();
                    if (scenario == "saved-recording-handoff") {
                        const auto recording = app.audio_session_coordinator_.Begin(AudioSessionMode::kLocalRecording);
                        assert(recording && !app.StopWatchPairing(0) && HasReservation());
                        forget_gate.Unblock(); ReachApp(app, Phase::kControlRemoved);
                        assert(app.audio_session_coordinator_.Generation() == recording.generation);
                        assert(app.audio_session_coordinator_.Complete(recording));
                    } else {
                        assert(!app.StopWatchPairing(0) && HasReservation()); forget_gate.Unblock();
                    }
                }
                ReachApp(app, scenario == "saved-reject" ? Phase::kCancelled : Phase::kControlRemoved);
            }
            }
        }
        assert(!app.watch_pairing_->Active() && !power.storage_leases && !power.network_leases && !Blufi::GetInstance().lease);
        assert(!http_requests && !identity_reads && !writes && !commits && !live_window);
    } else if (scenario.rfind("forget-", 0) == 0) {
        forget_allowed = true;
        local_control_grant = lease;
        app.audio_service_.enabled = false;
        app.protocol_ = std::make_unique<TestProtocol>();
        const auto* protocol = app.protocol_.get();
        WifiStation::GetInstance().connected = false;
        GetWatchWifiGate().Initialize(false);
        identity_present = encrypted = tls_ok = false;
        unix_seconds = 0;
        if (scenario == "forget-enabled") app.audio_service_.enabled = true;
        if (scenario == "forget-busy") app.watch_library_request_ = 8;
        if (scenario == "forget-starting") Board::GetInstance().starting = true;
        if (scenario == "forget-storage-lease") power.allowed = false;
        if (scenario == "forget-ble-lease") Blufi::GetInstance().lease_allowed = false;
        auto open = Request(app, WatchViewAction::kOpenControlRemoval);
        open.pairing_peer_generation = lease;
        if (scenario == "forget-queued-cancel") {
            auto cancel = Request(app, WatchViewAction::kCancelDevicePairing);
            cancel.pairing_open_request = open.id;
            app.ObserveWatchPairingIntent(cancel);
        }
        if (scenario == "forget-enabled" || scenario == "forget-busy" || scenario == "forget-starting" ||
            scenario == "forget-storage-lease" || scenario == "forget-ble-lease" || scenario == "forget-queued-cancel") {
            assert(!app.HandleWatchPairingRequest(open));
            assert(!forget_calls);
        } else {
            assert(app.PollWatchPairing().forget_peer_generation == lease);
            auto invalid = open;
            invalid.pairing_peer_generation = 0;
            assert(!app.HandleWatchPairingRequest(invalid));
            invalid.pairing_peer_generation = lease + 1;
            assert(!app.HandleWatchPairingRequest(invalid));
            assert(app.HandleWatchPairingRequest(open));
            auto pending = app.PollWatchPairing();
            assert(pending.state.phase == Phase::kAwaitControlRemoval && HasReservation());
            assert(!pending.network_required && !pending.forget_peer_generation && !forget_calls);
            auto confirm = Request(app, WatchViewAction::kConfirmControlRemoval, true);
            confirm.pairing_peer_generation = lease;
            auto stale = confirm;
            stale.pairing_peer_generation = lease + 1;
            stale.enabled = false;
            app.ObserveWatchPairingIntent(stale);
            assert(!app.HandleWatchPairingRequest(stale) && !app.watch_pairing_revoked_.load());
            auto off = Request(app, WatchViewAction::kSetWifiEnabled, false);
            app.ObserveWatchPairingIntent(off);
            assert(!app.watch_pairing_revoked_.load());
            if (scenario == "forget-reject" || scenario == "forget-enable-cancel") {
                if (scenario == "forget-reject") {
                    confirm.enabled = false;
                    app.ObserveWatchPairingIntent(confirm);
                    assert(app.HandleWatchPairingRequest(confirm));
                } else {
                    auto enable = Request(app, WatchViewAction::kSetVoiceEnabled, true);
                    app.ObserveWatchPairingIntent(enable);
                    assert(!app.HandleWatchPairingRequest(confirm));
                }
                ReachApp(app, Phase::kCancelled);
                assert(!forget_calls);
            } else {
                const bool gated = scenario == "forget-cancel-active" || scenario == "forget-recording-handoff";
                if (gated) forget_gate.Block();
                if (scenario == "forget-failed") forget_error = WatchControlRemovalError::kNative;
                assert(app.HandleWatchPairingRequest(confirm));
                if (gated) {
                    forget_gate.Await();
                    if (scenario == "forget-recording-handoff") {
                        const auto recording = app.audio_session_coordinator_.Begin(AudioSessionMode::kLocalRecording);
                        assert(recording && !app.StopWatchPairing(0) && HasReservation());
                        forget_gate.Unblock();
                        ReachApp(app, Phase::kControlRemoved);
                        assert(app.audio_session_coordinator_.Generation() == recording.generation);
                        assert(app.audio_session_coordinator_.Complete(recording));
                    } else {
                        auto cancel = Request(app, WatchViewAction::kCancelDevicePairing);
                        app.ObserveWatchPairingIntent(cancel);
                        assert(app.HandleWatchPairingRequest(cancel) && HasReservation());
                        forget_gate.Unblock();
                    }
                }
                ReachApp(app, scenario == "forget-failed" ? Phase::kFailed : Phase::kControlRemoved);
                assert(forget_calls == 1);
                if (scenario == "forget-failed") assert(app.PollWatchPairing().state.error == Error::kControlRemoval);
            }
        }
        assert(app.protocol_.get() == protocol);
        assert(!identity_reads && !http_requests && !storage_opens && !writes && !commits);
        assert(!app.watch_pairing_->Active());
        if (scenario != "forget-recording-handoff") assert(app.audio_session_coordinator_.Mode() == AudioSessionMode::kIdle);
    } else if (scenario.rfind("remove-", 0) == 0) {
        assert(watch_voice::SaveBinding(0, device, credential) == watch_voice::BindingStatus::kBound);
        writes = commits = 0;
        app.audio_service_.enabled = false;
        WifiStation::GetInstance().connected = false;
        GetWatchWifiGate().Initialize(false);
        native_host_ready = Blufi::GetInstance().profile_ready = false;
        identity_present = tls_ok = false;
        unix_seconds = 0;
        auto open = Request(app, WatchViewAction::kOpenBindingRemoval);
        if (scenario == "remove-enabled") app.audio_service_.enabled = true;
        if (scenario == "remove-busy") app.watch_library_request_ = 8;
        if (scenario == "remove-storage-failure") power.allowed = false;
        if (scenario == "remove-protocol-busy") {
            app.protocol_ = std::make_unique<TestProtocol>();
            app.protocol_->close_ready = false;
        }
        if (scenario == "remove-queued-cancel") {
            auto cancel = Request(app, WatchViewAction::kCancelDevicePairing);
            cancel.pairing_open_request = open.id;
            app.ObserveWatchPairingIntent(cancel);
        }
        if (scenario == "remove-enabled" || scenario == "remove-busy" || scenario == "remove-storage-failure" ||
            scenario == "remove-protocol-busy" || scenario == "remove-queued-cancel") {
            assert(!app.HandleWatchPairingRequest(open));
            if (scenario == "remove-protocol-busy") assert(app.protocol_ && !app.protocol_->close_ready);
            assert(writes == 0 && commits == 0);
        } else {
            app.protocol_ = std::make_unique<TestProtocol>();
            assert(app.PollWatchPairing().can_remove_binding);
            assert(app.HandleWatchPairingRequest(open));
            ReachApp(app, Phase::kAwaitRemoval);
            const auto pending = app.PollWatchPairing();
            assert(pending.active && pending.state.removing_binding && !pending.network_required);
            assert(!pending.can_remove_binding && !pending.can_open && !pending.can_authorize_cloud);
            assert(HasReservation() && !app.protocol_);
            auto confirm = Request(app, WatchViewAction::kConfirmBindingRemoval, true);
            confirm.binding_generation = pending.state.binding_generation;
            assert(confirm.binding_generation == 1);
            if (scenario == "remove-enable-cancel") {
                auto enable = Request(app, WatchViewAction::kSetVoiceEnabled, true);
                app.ObserveWatchPairingIntent(enable);
                assert(!app.HandleWatchPairingRequest(confirm));
                ReachApp(app, Phase::kCancelled);
            } else if (scenario == "remove-reject") {
                confirm.enabled = false;
                app.ObserveWatchPairingIntent(confirm);
                assert(app.HandleWatchPairingRequest(confirm));
                ReachApp(app, Phase::kCancelled);
            } else {
                auto stale = confirm;
                ++stale.binding_generation;
                stale.enabled = false;
                app.ObserveWatchPairingIntent(stale);
                assert(!app.watch_pairing_revoked_.load() && !app.HandleWatchPairingRequest(stale));
                auto off = Request(app, WatchViewAction::kSetWifiEnabled, false);
                app.ObserveWatchPairingIntent(off);
                assert(!app.watch_pairing_revoked_.load());
                if (scenario == "remove-cancel-write") write_gate.Block();
                assert(app.HandleWatchPairingRequest(confirm));
                if (scenario == "remove-cancel-write") {
                    write_gate.Await();
                    auto cancel = Request(app, WatchViewAction::kCancelDevicePairing);
                    app.ObserveWatchPairingIntent(cancel);
                    assert(app.HandleWatchPairingRequest(cancel) && HasReservation());
                    write_gate.Unblock();
                }
                ReachApp(app, Phase::kBindingRemoved);
                assert(app.PollWatchPairing().state.binding_generation == 2);
                assert(writes == 1 && commits == 1);
            }
        }
        watch_voice::Binding binding;
        const bool removed = scenario == "remove-success" || scenario == "remove-cancel-write";
        assert(watch_voice::LoadBinding(binding) == (removed
            ? watch_voice::BindingStatus::kRemoved : watch_voice::BindingStatus::kBound));
        assert(binding.generation == (removed ? 2U : 1U));
        sodium_memzero(binding.credential.data(), binding.credential.size());
        assert(!identity_reads && !http_requests && !live_window);
        assert(app.audio_session_coordinator_.Mode() == AudioSessionMode::kIdle);
    } else if (scenario == "native-not-ready" || scenario == "profile-not-ready") {
        native_host_ready = scenario != "native-not-ready";
        Blufi::GetInstance().profile_ready = scenario != "profile-not-ready";
        for (bool local : {false, true}) {
            assert(!app.CanOpenWatchPairing(local));
            assert(!app.PollWatchPairing().can_open);
            auto request = Request(app, WatchViewAction::kOpenDevicePairing);
            request.pairing_control_only = local;
            assert(!app.HandleWatchPairingRequest(request));
            assert(!HasReservation() && app.audio_session_coordinator_.Mode() == AudioSessionMode::kIdle);
            assert(identity_reads == 0 && http_requests == 0 && !live_window);
        }
        native_host_ready = true;
        Blufi::GetInstance().profile_ready = true;
        assert(app.PollWatchPairing().can_open);
        auto request = Request(app, WatchViewAction::kOpenDevicePairing);
        request.pairing_control_only = true;
        assert(app.HandleWatchPairingRequest(request));
        assert(!app.PollWatchPairing().can_open);
        assert(app.StopWatchPairing(0));
    } else if (scenario == "local-offline" || scenario == "local-cancel" || scenario == "local-recording" || scenario == "local-reconnect") {
        WifiStation::GetInstance().connected = false;
        GetWatchWifiGate().Initialize(false);
        identity_present = encrypted = tls_ok = false; unix_seconds = 0;
        assert(app.PollWatchPairing().can_open && !app.PollWatchPairing().network_required);
        auto open = Request(app, WatchViewAction::kOpenDevicePairing);
        open.pairing_control_only = true;
        assert(app.HandleWatchPairingRequest(open));
        const auto pending = app.PollWatchPairing();
        assert(pending.active && pending.state.control_only && pending.confirmation == 1);
        auto confirm = Request(app, WatchViewAction::kConfirmDevicePairingPeer, true);
        confirm.pairing_confirmation = pending.confirmation;
        assert(app.HandleWatchPairingRequest(confirm));
        if (scenario == "local-cancel") {
            auto cancel = Request(app, WatchViewAction::kCancelDevicePairing);
            app.ObserveWatchPairingIntent(cancel); assert(app.HandleWatchPairingRequest(cancel));
            assert(app.PollWatchPairing().state.phase == Phase::kCancelled);
        } else if (scenario == "local-recording") {
            const auto recording = app.audio_session_coordinator_.Begin(AudioSessionMode::kLocalRecording);
            assert(recording && app.StopWatchPairing(0));
            assert(app.audio_session_coordinator_.Generation() == recording.generation);
        } else {
            auto off = Request(app, WatchViewAction::kSetWifiEnabled, false);
            app.ObserveWatchPairingIntent(off); GetWatchWifiGate().Observe(off.id, false);
            assert(!app.watch_pairing_revoked_.load() && app.watch_pairing_->Active());
            local_control_grant = live_window; live_window = 0; local_control_mode = false;
            auto ready = app.PollWatchPairing();
            assert(ready.state.phase == Phase::kControlReady && ready.state.control_peer_generation == lease);
            assert(!ready.active && !ready.can_open && !HasReservation());
            assert(!ready.state.binding_generation && ready.state.device_id.empty());
            if (scenario == "local-reconnect") {
                local_control_grant = 0; peer_confirmed = false;
                ready = app.PollWatchPairing();
                assert(ready.state.phase == Phase::kClosed && ready.can_open && !ready.state.control_peer_generation);
                open = Request(app, WatchViewAction::kOpenDevicePairing); open.pairing_control_only = true;
                assert(app.HandleWatchPairingRequest(open));
                assert(app.StopWatchPairing(0));
            }
        }
        assert(storage_opens == 0 && identity_reads == 0 && http_requests == 0 && writes == 0 && commits == 0);
    } else if (scenario == "success") {
        VerifyApp(app); ReachApp(app, Phase::kBound); AssertSaved();
        assert(app.PollWatchPairing().state.binding_generation == 1);
        now += 5000; app.PollWatchPairing();
        assert(!app.watch_pairing_->Active() && app.audio_session_coordinator_.Mode() == AudioSessionMode::kIdle);
    } else if (scenario == "offline" || scenario == "provisioning" || scenario == "wake-running" ||
               scenario == "busy" || scenario == "storage-failure" || scenario == "network-lease-failure" || scenario == "ble-lease-failure") {
        if (scenario == "offline") WifiStation::GetInstance().connected = false;
        if (scenario == "provisioning") Blufi::GetInstance().provisioning = true;
        if (scenario == "wake-running") app.audio_service_.wake_running = true;
        if (scenario == "busy") app.watch_library_request_ = 8;
        if (scenario == "storage-failure") power.allowed = false;
        if (scenario == "network-lease-failure") power.state = AudreamPowerState::kDeepSleepPreparing;
        if (scenario == "ble-lease-failure") Blufi::GetInstance().lease_allowed = false;
        auto request = Request(app, WatchViewAction::kOpenDevicePairing);
        assert(!app.HandleWatchPairingRequest(request));
        assert(app.audio_session_coordinator_.Mode() == AudioSessionMode::kIdle);
        if (scenario == "offline") {
            assert(!app.PollWatchPairing().network_required && app.PollWatchPairing().can_open);
            assert(app.CanOpenWatchPairing(true));
        }
        AssertNoBinding();
    } else if (scenario == "cancel-queued-open") {
        const auto open = Request(app, WatchViewAction::kOpenDevicePairing);
        auto cancel = Request(app, WatchViewAction::kCancelDevicePairing);
        cancel.pairing_open_request = open.id;
        app.ObserveWatchPairingIntent(cancel);
        assert(!app.HandleWatchPairingRequest(open));
        assert(!app.HandleWatchPairingRequest(cancel));
        assert(identity_reads == 0); AssertNoBinding();
    } else if (scenario == "stale-cancel") {
        const auto old = OpenApp(app);
        auto cancel = Request(app, WatchViewAction::kCancelDevicePairing);
        cancel.pairing_open_request = old.id;
        app.ObserveWatchPairingIntent(cancel); assert(app.HandleWatchPairingRequest(cancel));
        ReachApp(app, Phase::kCancelled);
        OpenApp(app);
        app.ObserveWatchPairingIntent(cancel); assert(!app.HandleWatchPairingRequest(cancel));
        assert(!app.watch_pairing_revoked_.load() && app.watch_pairing_->Active());
        cancel.pairing_attempt = app.watch_pairing_->Snapshot().attempt;
        app.ObserveWatchPairingIntent(cancel); assert(!app.HandleWatchPairingRequest(cancel));
        assert(!app.watch_pairing_revoked_.load());
        app.StopWatchPairing(1000); AssertNoBinding();
    } else if (scenario == "reject-after-approval" || scenario == "stale-rejection") {
        auto approval = OfferApp(app);
        assert(app.HandleWatchPairingRequest(approval)); ReachApp(app, Phase::kAwaitCredential);
        auto reject = approval; reject.id = ++next_ui; reject.enabled = false;
        if (scenario == "stale-rejection") ++reject.pairing_request;
        app.ObserveWatchPairingIntent(reject);
        if (scenario == "stale-rejection") {
            assert(!app.HandleWatchPairingRequest(reject) && !app.watch_pairing_revoked_.load());
            app.StopWatchPairing(1000);
        } else {
            assert(app.watch_pairing_revoked_.load() && app.HandleWatchPairingRequest(reject));
            ReachApp(app, Phase::kCancelled);
        }
        AssertNoBinding();
    } else if (scenario == "cancel-during-write" || scenario == "cancel-commit-failed") {
        fail_commit = scenario == "cancel-commit-failed";
        write_gate.Block(); VerifyApp(app); write_gate.Await();
        auto cancel = Request(app, WatchViewAction::kCancelDevicePairing);
        app.ObserveWatchPairingIntent(cancel); assert(app.HandleWatchPairingRequest(cancel));
        assert(HasReservation() && app.watch_pairing_->Active());
        write_gate.Unblock(); ReachApp(app, fail_commit ? Phase::kFailed : Phase::kBound);
        Wait([&] { app.PollWatchPairing(); return !app.watch_pairing_->Active(); });
        if (fail_commit) assert(app.PollWatchPairing().state.error == Error::kStorage);
        else { AssertSaved(); assert(app.PollWatchPairing().state.binding_generation == 1); }
    } else if (scenario == "wifi-off" || scenario == "recording-handoff" || scenario == "handoff-timeout") {
        http_gate.Block(); VerifyApp(app); http_gate.Await();
        if (scenario == "wifi-off") {
            auto off = Request(app, WatchViewAction::kSetWifiEnabled, false);
            app.ObserveWatchPairingIntent(off); GetWatchWifiGate().Observe(off.id, false);
            assert(!app.StopWatchPairing(0) && HasReservation());
            http_gate.Unblock(); ReachApp(app, Phase::kCancelled);
        } else {
            const auto recording = app.audio_session_coordinator_.Begin(AudioSessionMode::kLocalRecording);
            assert(recording && !app.audio_session_coordinator_.OwnsDevicePairingResources(app.watch_pairing_audio_generation_));
            std::mutex queue_mutex;
            std::deque<std::function<void()>> queue;
            AssistantTransportHandoff handoff(
                [&](std::function<void()> work) { std::lock_guard<std::mutex> lock(queue_mutex); queue.push_back(std::move(work)); },
                [] { return std::this_thread::get_id() == owner; },
                [&](uint64_t generation) { return app.audio_session_coordinator_.HoldsExclusiveResources(generation); },
                [&](uint32_t timeout) { return app.StopWatchPairing(timeout); });
            bool closed = false;
            std::thread recorder([&] { closed = handoff.CloseWithin(recording.generation, scenario == "handoff-timeout" ? 10 : 1000); });
            Wait([&] { std::lock_guard<std::mutex> lock(queue_mutex); return !queue.empty(); });
            if (scenario == "handoff-timeout") recorder.join();
            else http_gate.Unblock();
            std::function<void()> work;
            { std::lock_guard<std::mutex> lock(queue_mutex); work = std::move(queue.front()); queue.pop_front(); }
            work();
            if (recorder.joinable()) recorder.join();
            if (scenario == "handoff-timeout") {
                assert(!closed && HasReservation());
                assert(app.audio_session_coordinator_.Fail(recording, false));
                http_gate.Unblock(); ReachApp(app, Phase::kCancelled);
                assert(app.audio_session_coordinator_.Mode() == AudioSessionMode::kFault);
            } else assert(closed);
            assert(app.audio_session_coordinator_.HoldsExclusiveResources(recording.generation));
            if (scenario != "handoff-timeout") assert(app.audio_session_coordinator_.Complete(recording));
        }
        AssertNoBinding();
    } else assert(false);
    assert(!app.watch_pairing_->Active() && power.storage_leases == 0 && power.network_leases == 0 && !http_live);
    assert(Blufi::GetInstance().lease == 0);
    assert(!app.audio_service_.wake_running || scenario == "wake-running");
    std::cout << "PASS: pairing application " << scenario << '\n';
}
