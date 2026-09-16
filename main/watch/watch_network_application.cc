#include "application.h"
#include "board.h"
#include "watch/watch_wifi_gate.h"
#include "wifi_manager.h"
#include "system/audream_power_manager.h"
#include "esp_timer.h"
#include "esp_log.h"

bool Application::CanPrepareWatchNetwork() {
    const bool authorized = watch_cloud_required_
        ? watch_cloud_authorization_ && watch_cloud_authorization_->Usable(esp_timer_get_time() / 1000)
        : protocol_ != nullptr;
    return !WatchFirmwareBusy() && !watch_network_preparing_ && audio_service_.IsAssistantUserEnabled() &&
        GetWatchWifiGate().Enabled() && !GetWatchWifiGate().TransferActive() &&
        (!watch_cloud_required_ || watch_voice::ActiveBindingGeneration()) &&
        Board::GetInstance().HasWatchWifiCredentials() &&
        !Board::GetInstance().IsWatchWifiProvisioning() &&
        audio_session_coordinator_.Mode() == AudioSessionMode::kIdle && audio_service_.IsIdle() &&
        !Board::GetInstance().GetWatchRecordingControlStatus().starting && !HasAssistantSession() &&
        !pending_assistant_listening_ && !assistant_session_.IsClosing() && !reset_protocol_pending_ &&
        !watch_cloud_task_.Active() && !watch_cloud_application_ && !watch_cloud_ack_ &&
        !watch_cloud_retirement_ && !watch_wake_retirement_ && !watch_power_change_ && !watch_deferred_wifi_request_ &&
        (!GetWatchWifiGate().StationAllowed() || !WifiManager::GetInstance().IsConnected() || !authorized);
}

bool Application::RequestWatchNetwork() {
    std::lock_guard<std::mutex> lock(watch_voice_mutex_);
    if (!CanPrepareWatchNetwork()) return false;
    const auto now = esp_timer_get_time();
    watch_network_preparing_ = true;
    watch_network_failed_ = false;
    watch_wifi_change_failed_ = false;
    watch_network_deadline_us_ = now + 90000000;
    watch_network_binding_generation_ = watch_voice::ActiveBindingGeneration();
    watch_network_voice_intent_ = watch_voice_request_;
    watch_station_policy_.Request(now / 1000);
    GetWatchWifiGate().SetStationAllowed(true);
    watch_wake_lease_.Revoke();
    watch_cloud_authorization_.reset();
    watch_power_mode_ = {};
    watch_cloud_retry_after_us_ = 0;
    return true;
}

void Application::CancelWatchNetwork() {
    if (watch_network_preparing_) {
        watch_cloud_task_.Cancel();
        watch_cloud_ack_.reset();
        if (!watch_always_available_) GetWatchWifiGate().SetStationAllowed(false);
    }
    watch_network_preparing_ = false;
    watch_station_policy_.CancelDemand();
}

void Application::PollWatchNetwork() {
    auto& gate = GetWatchWifiGate();
    auto& board = Board::GetInstance();
    const auto now = esp_timer_get_time();
    const auto mode = audio_session_coordinator_.Mode();
    const bool provisioning = board.IsWatchWifiProvisioning();
    if (provisioning && gate.Enabled() && !gate.TransferActive()) {
        gate.SetStationAllowed(true);
        watch_station_policy_.Request(now / 1000);
    }
    {
        std::lock_guard<std::mutex> lock(watch_voice_mutex_);
        if (watch_network_preparing_ &&
            (watch_network_binding_generation_ != watch_voice::ActiveBindingGeneration() ||
             watch_network_voice_intent_ != watch_voice_request_ || !audio_service_.IsAssistantUserEnabled() ||
             !gate.Enabled() || gate.TransferActive() || now >= watch_network_deadline_us_ ||
             (mode != AudioSessionMode::kIdle && !watch_cloud_application_ && !watch_cloud_retirement_) ||
             board.GetWatchRecordingControlStatus().starting)) {
            watch_network_failed_ = now >= watch_network_deadline_us_;
            CancelWatchNetwork();
        }
    }
    const bool connected = gate.StationAllowed() && WifiManager::GetInstance().IsConnected();
    if (watch_network_preparing_ && connected && !watch_cloud_task_.Active() &&
        !watch_cloud_ack_ && !watch_cloud_application_ && !watch_cloud_retirement_ &&
        (watch_cloud_required_ ? watch_power_mode_.revision != 0 : protocol_ != nullptr))
        watch_network_preparing_ = false;
    if (HasAssistantSession() || pending_assistant_listening_)
        watch_station_policy_.KeepForConversation(now / 1000);
    const bool busy = mode != AudioSessionMode::kIdle || !audio_service_.IsIdle() ||
        board.GetWatchRecordingControlStatus().starting || HasAssistantSession() || pending_assistant_listening_ ||
        assistant_session_.IsClosing() || reset_protocol_pending_ || gate.TransferActive() ||
        watch_cloud_task_.Active() || watch_cloud_application_ || watch_power_change_ ||
        watch_wake_retirement_ || watch_cloud_retirement_ || watch_deferred_wifi_request_;
    const auto action = watch_station_policy_.Poll(now / 1000, gate.Enabled(), watch_always_available_,
        busy, connected, provisioning);
    if (action == WatchStationAction::kNone) return;
    auto& power = AudreamPowerManager::GetInstance();
    if (power.State() == AudreamPowerState::kDeepSleepPreparing ||
        (action == WatchStationAction::kStart && !WatchCloudPowerReady() && !WatchFirmwareBusy())) return;
    if (action == WatchStationAction::kStop) {
        gate.SetStationAllowed(false);
        watch_station_recovery_.Cancel();
        watch_wake_lease_.Revoke();
        watch_wake_confirmed_revision_ = 0;
        watch_cloud_authorization_.reset();
        watch_cloud_ack_.reset();
        if (protocol_) {
            const auto reservation = audio_session_coordinator_.Begin(AudioSessionMode::kMaintenance);
            if (reservation) watch_cloud_retirement_ = reservation;
            return;
        }
    }
    const auto reservation = audio_session_coordinator_.Begin(AudioSessionMode::kMaintenance);
    if (!reservation) return;
    bool succeeded = false;
    if (power.BeginStorageRead()) {
        if (action == WatchStationAction::kStart) {
            gate.SetStationAllowed(true);
            succeeded = board.HasWatchWifiCredentials() && board.RestoreWatchStation();
        } else succeeded = board.SuspendIdleWatchStation();
        power.EndStorageRead();
    }
    audio_session_coordinator_.Fail(reservation, true);
    watch_station_policy_.Complete(action, succeeded, now / 1000);
    if (watch_network_preparing_ && watch_station_policy_.Failed()) {
        watch_network_failed_ = true;
        CancelWatchNetwork();
    }
    if (!succeeded) watch_wifi_change_failed_ = true;
    ESP_LOGI("WatchNetwork", "Station policy operation: action=%s accepted=%d",
        action == WatchStationAction::kStart ? "start" : "stop", succeeded);
}
