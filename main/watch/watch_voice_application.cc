#include "application.h"
#include "board.h"
#include "display.h"
#include "watch/watch_view_state.h"
#include "watch/watch_wifi_gate.h"
#include "wifi_station.h"
#include "system/audream_power_manager.h"
#include "system/device_time_service.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "assets.h"
#include <sys/time.h>

namespace {
constexpr char kTag[] = "WatchVoice";
constexpr int64_t kRetryIntervalUs = 60000000;
}

watch_voice::Endpoints Application::WatchVoiceEndpoints() {
#if CONFIG_AUDREAM_WATCH_UI
    return {CONFIG_AUDREAM_VOICE_API_ORIGIN, CONFIG_AUDREAM_VOICE_WEBSOCKET_URL};
#else
    return {};
#endif
}

void Application::PollWatchAssetHealth(bool healthy) {
    if (!watch_asset_health_active_) return;
    auto& power = AudreamPowerManager::GetInstance();
    const bool eligible = healthy && watch_boot_health_.Confirmed() && audio_service_.IsWakeWordRunning();
    const bool leased = eligible && power.BeginStorageRead();
    watch_asset_health_active_ = Assets::GetInstance().PollTrialHealth(
        esp_timer_get_time() / 1000, leased, audio_service_.WakeInputFeedCount());
    if (leased) power.EndStorageRead();
}

bool Application::PrepareWatchTlsClock() {
    auto& clock = DeviceTimeService::GetInstance();
    if (!clock.IsSynchronized()) return false;
    const auto milliseconds = clock.CurrentUnixTimeMs();
    if (milliseconds < 1600000000000ULL || milliseconds > 4102444800000ULL) return false;
    timeval time{};
    time.tv_sec = static_cast<time_t>(milliseconds / 1000);
    time.tv_usec = static_cast<suseconds_t>((milliseconds % 1000) * 1000);
    // DeviceTime is the watch's provisioned clock; TLS uses the POSIX clock.
    // Never bypass certificate validity checks when no usable clock exists.
    return settimeofday(&time, nullptr) == 0;
}

namespace {
bool CurrentBinding(const std::string& device_id, uint64_t generation, watch_voice::Binding& binding) {
    auto& power = AudreamPowerManager::GetInstance();
    if (!power.BeginStorageRead()) return false;
    const auto status = watch_voice::LoadBinding(binding);
    power.EndStorageRead();
    return status == watch_voice::BindingStatus::kBound && binding.device_id == device_id &&
        binding.generation == generation;
}
}

bool Application::WatchCloudPowerReady() const {
    const auto state = AudreamPowerManager::GetInstance().State();
    return state == AudreamPowerState::kActive ||
        (watch_always_available_ && state == AudreamPowerState::kIdle);
}

WatchPowerModeSnapshot Application::BuildWatchPowerModeSnapshot() {
    auto state = watch_power_mode_;
    const auto generation = watch_voice::ActiveBindingGeneration();
    if (!generation || state.binding_generation != generation) return {};
    state.pending = watch_power_change_.has_value() || watch_cloud_application_.has_value() ||
        watch_cloud_ack_.has_value();
    state.can_change = !WatchFirmwareBusy() && watch_cloud_required_ && state.revision && !state.pending &&
        !watch_cloud_task_.Active() && !watch_cloud_retirement_ && !watch_wake_retirement_ &&
        GetWatchWifiGate().Enabled() && GetWatchWifiGate().StationAllowed() && WifiStation::GetInstance().IsConnected() &&
        !GetWatchWifiGate().TransferActive() && WatchCloudPowerReady() &&
        audio_session_coordinator_.Mode() == AudioSessionMode::kIdle &&
        (audio_service_.IsWakeWordRunning() || audio_service_.IsIdle()) &&
        !Board::GetInstance().GetWatchRecordingControlStatus().starting && !HasAssistantSession() &&
        !pending_assistant_listening_ && !assistant_session_.IsClosing() &&
        !watch_deferred_wifi_request_ && !reset_protocol_pending_;
    return state;
}

bool Application::RequestWatchPowerMode(uint64_t revision, uint64_t binding_generation, const std::string& mode) {
    std::lock_guard<std::mutex> lock(watch_voice_mutex_);
    const auto state = BuildWatchPowerModeSnapshot();
    if (!state.can_change || revision != state.revision || binding_generation != state.binding_generation ||
        watch_voice::EncodePowerModeRequest(revision, mode).empty()) return false;
    watch_power_change_ = WatchPowerModeChange{revision, binding_generation,
        audio_session_coordinator_.Generation(), watch_voice_request_, mode,
        esp_timer_get_time() + 30000000, false};
    watch_power_mode_.message = WatchPowerModeMessage::kSaving;
    watch_station_policy_.Request(esp_timer_get_time() / 1000);
    watch_wake_lease_.Revoke();
    watch_wake_confirmed_revision_ = 0;
    watch_cloud_retry_after_us_ = 0;
    return true;
}

void Application::CancelWatchPowerMode(watch_voice::Result result) {
    watch_cloud_task_.Cancel();
    watch_power_change_.reset();
    watch_power_mode_ = {};
    watch_power_mode_.binding_generation = watch_voice::ActiveBindingGeneration();
    watch_power_mode_.message = result == watch_voice::Result::kConflict
        ? WatchPowerModeMessage::kConflict : WatchPowerModeMessage::kFailed;
    // A POST may have committed before cancellation. Reconcile with a GET/bootstrap,
    // never retry the mutation against a newly read revision without another gesture.
    watch_cloud_retry_after_us_ = 0;
}

void Application::StartWatchBootstrap(const watch_voice::Binding& binding) {
    const auto now = esp_timer_get_time();
    if (WatchFirmwareBusy() || watch_cloud_task_.Active() || watch_cloud_application_ || watch_cloud_ack_ || watch_wake_retirement_ ||
        now < watch_cloud_retry_after_us_ || protocol_ ||
        !GetWatchWifiGate().Enabled() || !GetWatchWifiGate().StationAllowed() ||
        !WatchCloudPowerReady() ||
        !WifiStation::GetInstance().IsConnected() || GetWatchWifiGate().TransferActive() ||
        audio_session_coordinator_.Mode() != AudioSessionMode::kIdle ||
        audio_service_.IsWakeWordRunning() || !audio_service_.IsIdle() || Board::GetInstance().GetWatchRecordingControlStatus().starting ||
        assistant_session_.IsClosing() || reset_protocol_pending_) return;
    watch_cloud_retry_after_us_ = now + kRetryIntervalUs;
    const auto endpoints = WatchVoiceEndpoints();
    if (!endpoints.Valid() || !PrepareWatchTlsClock()) {
        ESP_LOGW(kTag, "Bootstrap requires trusted endpoints and a synchronized device clock");
        return;
    }
    const auto audio_generation = audio_session_coordinator_.Generation();
    std::lock_guard<std::mutex> lock(watch_voice_mutex_);
    const bool local_permission = audio_service_.IsAssistantUserEnabled();
    const auto power_change = watch_power_change_;
    if (power_change && (power_change->voice_intent != watch_voice_request_ ||
        power_change->binding_generation != binding.generation ||
        power_change->audio_generation != audio_generation)) {
        CancelWatchPowerMode(watch_voice::Result::kCancelled);
        return;
    }
    watch_wake_lease_.Revoke();
    watch_wake_confirmed_revision_ = 0;
    const bool started = watch_cloud_task_.Start(
        [this, binding, endpoints, audio_generation, local_permission, power_change](const watch_voice::Client::Cancelled& cancelled,
                                                     watch_voice::Response& response) {
            auto& result = std::get<watch_voice::Bootstrap>(response);
            const auto invalidated = [this, cancelled, audio_generation, local_permission, binding, power_change]() {
                if (power_change) {
                    std::lock_guard<std::mutex> lock(watch_voice_mutex_);
                    if (watch_voice_request_ != power_change->voice_intent) return true;
                }
                return cancelled() || audio_service_.IsAssistantUserEnabled() != local_permission ||
                    watch_voice::ActiveBindingGeneration() != binding.generation ||
                    !GetWatchWifiGate().Enabled() || !GetWatchWifiGate().StationAllowed() || GetWatchWifiGate().TransferActive() ||
                    audio_session_coordinator_.Mode() != AudioSessionMode::kIdle ||
                    !audio_session_coordinator_.IsCurrent(audio_generation);
            };
            watch_voice::Client client(endpoints);
            if (power_change) {
                watch_voice::Settings saved;
                const auto status = client.UpdatePowerMode(binding.device_id, binding.credential,
                    power_change->revision, power_change->power_mode, true, invalidated, saved);
                if (status != watch_voice::Result::kOk) return status;
            }
            const auto status = client.FetchBootstrap(binding.device_id, binding.credential, 1, true, invalidated, result);
            if (status == watch_voice::Result::kOk && power_change &&
                (result.settings.revision != power_change->revision + 1 ||
                 result.settings.power_mode != power_change->power_mode)) {
                result = {};
                return watch_voice::Result::kConflict;
            }
            return status;
        }, [allow_idle = watch_always_available_]() { return AudreamPowerManager::GetInstance().BeginVoiceNetworkRequest(allow_idle); },
        []() { AudreamPowerManager::GetInstance().EndVoiceNetworkRequest(); });
    if (!started) {
        ESP_LOGW(kTag, "Bootstrap worker could not start");
        return;
    }
    if (watch_power_change_) watch_power_change_->sent = true;
    watch_cloud_authorization_.reset();
    watch_cloud_device_id_ = binding.device_id;
    watch_cloud_binding_generation_ = binding.generation;
    watch_cloud_request_audio_generation_ = audio_generation;
    watch_cloud_request_voice_revision_ = watch_voice_request_;
    ESP_LOGI(kTag, "Bootstrap worker started");
}

void Application::PollWatchPowerPolicy() {
    auto& power = AudreamPowerManager::GetInstance();
    const auto generation = watch_voice::ActiveBindingGeneration();
    if (watch_power_restored_ && watch_power_binding_generation_ == generation) return;
    if (watch_power_binding_generation_ != generation) {
        // A new owner does not inherit the previous owner's sleep policy.
        if (!power.SetIdleDeepSleepEnabled(false)) return;
        watch_power_restored_ = false;
        watch_always_available_ = false;
        watch_power_binding_generation_ = generation;
        watch_power_retry_after_us_ = 0;
    }
    const auto now = esp_timer_get_time();
    if (now < watch_power_retry_after_us_ || !power.BeginStorageRead()) return;
    watch_power_retry_after_us_ = now + kRetryIntervalUs;
    watch_voice::Binding binding;
    const auto bound = watch_voice::LoadBinding(binding);
    watch_voice::SettingsReceipt receipt;
    const auto loaded = bound == watch_voice::BindingStatus::kBound
        ? watch_voice::LoadSettingsReceipt(binding, receipt) : watch_voice::ReceiptStatus::kAbsent;
    const bool readable = bound == watch_voice::BindingStatus::kAbsent || bound == watch_voice::BindingStatus::kRemoved ||
        (bound == watch_voice::BindingStatus::kBound &&
         (loaded == watch_voice::ReceiptStatus::kOk || loaded == watch_voice::ReceiptStatus::kAbsent));
    if (readable) {
        const bool saver = loaded == watch_voice::ReceiptStatus::kOk && receipt.applied_revision &&
            receipt.applied_power_mode == "battery_saver";
        const auto apply = [&]() { return power.SetIdleDeepSleepEnabled(saver); };
        const bool applied = bound == watch_voice::BindingStatus::kBound
            ? watch_voice::WithBoundIdentity([&](std::string_view id, uint64_t current_generation) {
                return id == binding.device_id && current_generation == binding.generation && apply();
            }) : apply();
        if (applied) {
            watch_always_available_ = loaded == watch_voice::ReceiptStatus::kOk && receipt.applied_revision &&
                receipt.applied_power_mode == "always_available";
            watch_power_restored_ = true;
            watch_power_binding_generation_ = bound == watch_voice::BindingStatus::kBound ? binding.generation : 0;
            ESP_LOGI(kTag, "Stored power policy restored: battery_saver=%d", saver ? 1 : 0);
        }
    }
    power.EndStorageRead();
}

void Application::PollWatchBootstrap() {
    PollWatchPowerPolicy();
    if (watch_power_mode_.binding_generation != watch_voice::ActiveBindingGeneration()) watch_power_mode_ = {};
    {
        std::lock_guard<std::mutex> lock(watch_voice_mutex_);
        if (watch_power_change_ &&
            (watch_power_change_->binding_generation != watch_voice::ActiveBindingGeneration() ||
             watch_power_change_->voice_intent != watch_voice_request_ ||
             !audio_session_coordinator_.IsCurrent(watch_power_change_->audio_generation) ||
             esp_timer_get_time() >= watch_power_change_->deadline_us ||
             !GetWatchWifiGate().Enabled() || !GetWatchWifiGate().StationAllowed() || !WifiStation::GetInstance().IsConnected() ||
             GetWatchWifiGate().TransferActive() || Board::GetInstance().GetWatchRecordingControlStatus().starting ||
             (watch_power_change_->sent && !watch_cloud_task_.Active()))) {
            CancelWatchPowerMode(watch_voice::Result::kCancelled);
        }
    }
    if (watch_cloud_task_.Active() || watch_cloud_application_ || watch_cloud_ack_ || watch_cloud_retirement_ ||
        !watch_cloud_authorization_ || !watch_cloud_authorization_->Usable(esp_timer_get_time() / 1000) ||
        !watch_cloud_authorization_->settings.SupportsWakeRuntime()) {
        watch_wake_lease_.Revoke();
    }
    // A recording or sleep handoff may join and consume a cancelled request.
    if (watch_cloud_ack_in_flight_ && !watch_cloud_task_.Active()) {
        watch_cloud_ack_in_flight_ = false;
        watch_cloud_ack_.reset();
    }
    if (watch_cloud_retirement_) {
        watch_wake_lease_.Revoke();
        if (!audio_service_.QuiesceAssistant(0)) return;
        watch_wake_started_ = false;
        if (protocol_ && !protocol_->CloseAudioChannelWithin(0)) return;
        protocol_.reset();
        audio_session_coordinator_.Fail(*watch_cloud_retirement_, true);
        watch_cloud_retirement_.reset();
    }
    if (!GetWatchWifiGate().Enabled() || !GetWatchWifiGate().StationAllowed() ||
        !WifiStation::GetInstance().IsConnected() ||
        audio_session_coordinator_.Mode() != AudioSessionMode::kIdle || GetWatchWifiGate().TransferActive()) {
        if (watch_cloud_task_.Active()) watch_cloud_task_.Cancel();
    }
    watch_voice::Result status;
    watch_voice::Response response;
    if (watch_cloud_task_.Poll(status, response)) {
        const bool power_change = watch_power_change_ && watch_power_change_->sent;
        if (power_change) {
            watch_power_change_.reset();
            if (status != watch_voice::Result::kOk) CancelWatchPowerMode(status);
            else watch_power_mode_.message = WatchPowerModeMessage::kNone;
        }
        if (watch_cloud_ack_in_flight_) {
            FinishWatchSettingsAcknowledgement(status, response);
        } else {
            const auto* bootstrap = std::get_if<watch_voice::Bootstrap>(&response);
            auto result = bootstrap ? *bootstrap : watch_voice::Bootstrap{};
            // Re-read the binding after joining: an old account's response is not
            // authorized merely because its HTTP request completed successfully.
            auto& power = AudreamPowerManager::GetInstance();
            watch_voice::Binding current;
            auto binding_status = watch_voice::BindingStatus::kStorageError;
            if (power.BeginStorageRead()) {
                binding_status = watch_voice::LoadBinding(current);
                power.EndStorageRead();
            }
            bool accepted = false;
            {
                std::lock_guard<std::mutex> lock(watch_voice_mutex_);
                accepted = status == watch_voice::Result::kOk && binding_status == watch_voice::BindingStatus::kBound &&
                    current.device_id == watch_cloud_device_id_ && current.generation == watch_cloud_binding_generation_ &&
                    watch_voice_request_ == watch_cloud_request_voice_revision_ &&
                    audio_session_coordinator_.IsCurrent(watch_cloud_request_audio_generation_) &&
                    bootstrap && GetWatchWifiGate().Enabled() && GetWatchWifiGate().StationAllowed() &&
                    WifiStation::GetInstance().IsConnected() && !GetWatchWifiGate().TransferActive() &&
                    !watch_deferred_wifi_request_;
                if (accepted) {
                    const auto reservation = audio_session_coordinator_.Begin(AudioSessionMode::kMaintenance);
                    accepted = reservation && reservation.generation == watch_cloud_request_audio_generation_ + 1;
                    if (accepted) {
                        watch_cloud_application_transition_ = reservation;
                        watch_cloud_application_ = std::move(result);
                    } else if (reservation) {
                        audio_session_coordinator_.Fail(reservation, true);
                    }
                }
            }
            if (accepted) {
                ESP_LOGI(kTag, "Bootstrap settings await audio quiescence before application");
            } else {
                if (power_change && status == watch_voice::Result::kOk)
                    CancelWatchPowerMode(watch_voice::Result::kCancelled);
                watch_cloud_authorization_.reset();
                ESP_LOGI(kTag, "Bootstrap authorization not applied: result=%d binding_status=%d local_permission=%d cloud_enabled=%d",
                    static_cast<int>(status), static_cast<int>(binding_status),
                    audio_service_.IsAssistantUserEnabled(), result.settings.enabled);
            }
        }
    }
    PollWatchSettingsApplication();
    if (watch_cloud_application_) return;
    if (watch_deferred_wifi_request_ && !watch_cloud_task_.Active() && !WatchFirmwareBusy()
#if CONFIG_AUDREAM_WATCH_DEVICE_PAIRING
        && (!watch_pairing_ || !watch_pairing_->Active())
#endif
    ) {
        watch_cloud_ack_.reset();
        auto request = std::move(watch_deferred_wifi_request_);
        HandleWatchViewRequest(*request);
        return;
    }
    if (watch_cloud_ack_) {
        StartWatchSettingsAcknowledgement();
        return;
    }
    // Refresh only while awake and idle. This timer does not wake a sleeping
    // watch, reopen Wi-Fi or keep an idle socket alive for synchronization.
    if (WatchFirmwareBusy() || !watch_cloud_required_ || watch_cloud_task_.Active() || watch_wake_retirement_ ||
        !GetWatchWifiGate().Enabled() || !GetWatchWifiGate().StationAllowed() ||
        !WatchCloudPowerReady() ||
        !WifiStation::GetInstance().IsConnected() || GetWatchWifiGate().TransferActive() ||
        esp_timer_get_time() < watch_cloud_retry_after_us_ ||
        audio_session_coordinator_.Mode() != AudioSessionMode::kIdle ||
        assistant_session_.IsClosing() || reset_protocol_pending_) return;
    if (protocol_) {
        const auto reservation = audio_session_coordinator_.Begin(AudioSessionMode::kMaintenance);
        if (!reservation) return;
        watch_cloud_retirement_ = reservation;
        if (watch_power_change_) watch_power_change_->audio_generation = reservation.generation;
        watch_wake_lease_.Revoke();
        watch_wake_confirmed_revision_ = 0;
        watch_cloud_authorization_.reset();
        return;
    }
    watch_cloud_authorization_.reset();
    watch_wake_confirmed_revision_ = 0;
    // Also back off missing/corrupt bindings without reading NVS every tick.
    watch_cloud_retry_after_us_ = esp_timer_get_time() + kRetryIntervalUs;
    watch_voice::Binding binding;
    auto& power = AudreamPowerManager::GetInstance();
    if (!power.BeginStorageRead()) return;
    const auto binding_status = watch_voice::LoadBinding(binding);
    power.EndStorageRead();
    if (binding_status != watch_voice::BindingStatus::kBound) return;
    watch_cloud_retry_after_us_ = 0;
    StartWatchBootstrap(binding);
}

void Application::PollWatchSettingsApplication() {
    if (!watch_cloud_application_ || !watch_cloud_application_transition_) return;
    // Do not release the ownership reservation while codec work is still live.
    if (!audio_service_.QuiesceAssistant(0)) return;
    watch_voice::Binding binding;
    const bool bound = CurrentBinding(watch_cloud_device_id_, watch_cloud_binding_generation_, binding);
    watch_voice::SettingsReceipt previous;
    auto receipt_status = watch_voice::ReceiptStatus::kStorageError;
    auto& power = AudreamPowerManager::GetInstance();
    if (bound && power.BeginStorageRead()) {
        receipt_status = watch_voice::LoadSettingsReceipt(binding, previous);
        power.EndStorageRead();
    }
    std::lock_guard<std::mutex> lock(watch_voice_mutex_);
    auto result = std::move(*watch_cloud_application_);
    watch_cloud_application_.reset();
    const bool current = bound && watch_voice_request_ == watch_cloud_request_voice_revision_ &&
        audio_session_coordinator_.IsCurrent(watch_cloud_application_transition_->generation) &&
        GetWatchWifiGate().Enabled() && GetWatchWifiGate().StationAllowed() && WifiStation::GetInstance().IsConnected() &&
        !GetWatchWifiGate().TransferActive() && !watch_deferred_wifi_request_ && !reset_protocol_pending_;
    watch_cloud_authorization_.reset();
    if (current) {
        watch_power_mode_.revision = result.settings.revision;
        watch_power_mode_.applied_revision = result.settings.applied_revision;
        watch_power_mode_.binding_generation = binding.generation;
        watch_power_mode_.power_mode = result.settings.power_mode;
        watch_power_mode_.apply_error = result.settings.apply_error;
        std::string error;
        const bool receipt_valid = receipt_status == watch_voice::ReceiptStatus::kOk ||
            receipt_status == watch_voice::ReceiptStatus::kAbsent;
        const bool revision_current = result.settings.revision >= previous.revision &&
            (result.settings.revision != previous.revision ||
             (result.settings.enabled == previous.enabled && result.settings.power_mode == previous.power_mode));
        if (!receipt_valid || !revision_current) error = "application_failed";
        else if (result.settings.power_mode != "watch_standby" && result.settings.power_mode != "battery_saver" &&
                 result.settings.power_mode != "always_available")
            error = "unsupported_mode";
        else if (result.settings.enabled) {
            if (!audio_service_.IsAssistantUserEnabled()) error = "local_disabled";
            else if ((!result.settings.SupportsManualRuntime() && !result.settings.SupportsWakeRuntime()) ||
                     (result.settings.power_mode == "always_available" && !result.settings.SupportsWakeRuntime()))
                error = "unsupported_mode";
            else if (!result.Usable(esp_timer_get_time() / 1000)) error = "application_failed";
            else {
                if (result.settings.SupportsWakeRuntime()) {
#if CONFIG_USE_AUDIO_PROCESSOR
                    srmodel_list_t* models = nullptr;
                    if (!watch_wake_guard_ready_) error = "application_failed";
                    else {
                        auto& assets = Assets::GetInstance();
                        const bool ready = assets.LoadAssistantModels(models) &&
                            audio_service_.PrepareWatchWakeWord(models) &&
                            audio_service_.PrepareVoiceProcessing(models, true);
                        watch_asset_health_active_ = assets.BeginTrialHealth(ready,
                            esp_timer_get_time() / 1000, audio_service_.WakeInputFeedCount());
                        if (!ready) error = "application_failed";
                    }
#else
                    error = "unsupported_mode";
#endif
                }
                if (error.empty()) {
                    watch_cloud_authorization_ = result;
                    InitializeProtocol();
                    if (!protocol_) {
                        watch_cloud_authorization_.reset();
                        error = "application_failed";
                    }
                }
            }
        }
        auto saved = watch_voice::ReceiptStatus::kStorageError;
        watch_voice::SettingsReceipt receipt;
        const bool previous_saver = power.IdleDeepSleepEnabled();
        bool power_applied = false;
        if (error.empty()) {
            power_applied = power.SetIdleDeepSleepEnabled(result.settings.power_mode == "battery_saver");
            if (!power_applied) {
                error = "application_failed";
                watch_cloud_authorization_.reset();
            }
        }
        if (receipt_valid && revision_current && power.BeginStorageRead()) {
            saved = watch_voice::SaveSettingsReceipt(binding, result.settings.revision,
                result.settings.enabled, error, receipt, result.settings.power_mode);
            power.EndStorageRead();
        }
        if (saved != watch_voice::ReceiptStatus::kOk) {
            if (power_applied && !power.SetIdleDeepSleepEnabled(previous_saver))
                ESP_LOGE(kTag, "Power policy rollback failed; settings application is unconfirmed");
            watch_cloud_authorization_.reset();
            error = "application_failed";
        } else if (power_applied) {
            watch_always_available_ = result.settings.power_mode == "always_available";
            watch_power_restored_ = true;
            watch_power_binding_generation_ = binding.generation;
        }
        if (revision_current && receipt_status != watch_voice::ReceiptStatus::kConflict &&
            saved != watch_voice::ReceiptStatus::kConflict) {
            watch_cloud_ack_ = watch_voice::PendingAcknowledgement{
                binding.device_id, binding.generation, result.settings.revision, watch_voice_request_,
                watch_cloud_application_transition_->generation, audio_service_.IsAssistantUserEnabled(),
                result.settings.enabled, error};
            watch_cloud_ack_retry_after_us_ = 0;
        }
        watch_cloud_retry_after_us_ = esp_timer_get_time() + kRetryIntervalUs;
        watch_power_mode_.apply_error = error;
        ESP_LOGI(kTag, "Settings application finished: enabled=%d error=%s", result.settings.enabled,
                 error.empty() ? "none" : error.c_str());
        ESP_LOGI(kTag, "Settings receipt: load=%d save=%d revision_current=%d",
                 static_cast<int>(receipt_status), static_cast<int>(saved), revision_current);
    }
    audio_session_coordinator_.Fail(*watch_cloud_application_transition_, true);
    watch_cloud_application_transition_.reset();
}

void Application::StartWatchSettingsAcknowledgement() {
    if (!watch_cloud_ack_ || watch_cloud_task_.Active()) return;
    if (esp_timer_get_time() < watch_cloud_ack_retry_after_us_ ||
        !WatchCloudPowerReady() ||
        !GetWatchWifiGate().Enabled() || !GetWatchWifiGate().StationAllowed() || !WifiStation::GetInstance().IsConnected() ||
        GetWatchWifiGate().TransferActive()) return;
    watch_voice::Binding binding;
    const auto pending = *watch_cloud_ack_;
    const bool bound = CurrentBinding(pending.device_id, pending.binding_generation, binding);
    std::lock_guard<std::mutex> lock(watch_voice_mutex_);
    if (!bound || pending.voice_intent != watch_voice_request_ ||
        pending.local_permission != audio_service_.IsAssistantUserEnabled() ||
        !audio_session_coordinator_.IsCurrent(pending.audio_generation) || reset_protocol_pending_ ||
        (pending.enabled && pending.error.empty() && (!protocol_ || !watch_cloud_authorization_ ||
         watch_cloud_authorization_->settings.revision != pending.revision ||
         !watch_cloud_authorization_->Usable(esp_timer_get_time() / 1000)))) {
        watch_cloud_ack_.reset();
        watch_cloud_authorization_.reset();
        watch_cloud_retry_after_us_ = 0;
        return;
    }
    if (audio_session_coordinator_.Mode() != AudioSessionMode::kIdle ||
        assistant_session_.IsClosing() || !audio_service_.IsIdle()) return;
    watch_cloud_ack_retry_after_us_ = esp_timer_get_time() + kRetryIntervalUs;
    const auto endpoints = WatchVoiceEndpoints();
    if (!endpoints.Valid() || !PrepareWatchTlsClock()) return;
    watch_cloud_ack_in_flight_ = watch_cloud_task_.Start(
        [this, binding, pending, endpoints](const watch_voice::Client::Cancelled& cancelled,
                                          watch_voice::Response& response) {
            response = watch_voice::Settings{};
            const auto invalidated = [this, pending, cancelled]() {
                return cancelled() || pending.local_permission != audio_service_.IsAssistantUserEnabled() ||
                    !audio_session_coordinator_.IsCurrent(pending.audio_generation) ||
                    audio_session_coordinator_.Mode() != AudioSessionMode::kIdle ||
                    !GetWatchWifiGate().Enabled() || !GetWatchWifiGate().StationAllowed() || !WifiStation::GetInstance().IsConnected() ||
                    GetWatchWifiGate().TransferActive();
            };
            return watch_voice::Client(endpoints).Acknowledge(binding.device_id, binding.credential,
                pending.revision, pending.error, true, invalidated, std::get<watch_voice::Settings>(response));
        }, [allow_idle = watch_always_available_]() { return AudreamPowerManager::GetInstance().BeginVoiceNetworkRequest(allow_idle); },
        []() { AudreamPowerManager::GetInstance().EndVoiceNetworkRequest(); });
}

void Application::FinishWatchSettingsAcknowledgement(watch_voice::Result status,
                                                    const watch_voice::Response& response) {
    watch_cloud_ack_in_flight_ = false;
    if (!watch_cloud_ack_) return;
    const auto pending = *watch_cloud_ack_;
    watch_voice::Binding binding;
    const bool bound = CurrentBinding(pending.device_id, pending.binding_generation, binding);
    std::lock_guard<std::mutex> lock(watch_voice_mutex_);
    const bool current = bound && watch_voice_request_ == pending.voice_intent &&
        audio_service_.IsAssistantUserEnabled() == pending.local_permission &&
        audio_session_coordinator_.IsCurrent(pending.audio_generation);
    const auto* settings = std::get_if<watch_voice::Settings>(&response);
    if (current && status == watch_voice::Result::kOk && settings &&
        settings->revision == pending.revision && settings->apply_error == pending.error &&
        (!pending.error.empty() || settings->applied_revision == pending.revision)) {
        ESP_LOGI(kTag, "Settings acknowledgement confirmed: revision=%llu error=%s",
            static_cast<unsigned long long>(pending.revision), pending.error.empty() ? "none" : pending.error.c_str());
        watch_cloud_ack_.reset();
        watch_power_mode_.applied_revision = settings->applied_revision;
        watch_power_mode_.apply_error = settings->apply_error;
        if (pending.error.empty() && watch_cloud_authorization_ &&
            watch_cloud_authorization_->settings.SupportsWakeRuntime()) {
            watch_wake_confirmed_revision_ = pending.revision;
            watch_wake_confirmed_intent_ = pending.voice_intent;
            watch_wake_retry_after_us_ = 0;
        }
        watch_cloud_retry_after_us_ = esp_timer_get_time() + kRetryIntervalUs;
        return;
    }
    if (!current || status == watch_voice::Result::kConflict || status == watch_voice::Result::kUnauthorized ||
        status == watch_voice::Result::kCancelled) {
        watch_cloud_ack_.reset();
        watch_cloud_authorization_.reset();
        watch_power_mode_.message = status == watch_voice::Result::kConflict
            ? WatchPowerModeMessage::kConflict : WatchPowerModeMessage::kFailed;
        watch_cloud_retry_after_us_ = esp_timer_get_time() + kRetryIntervalUs;
    } else {
        watch_cloud_ack_retry_after_us_ = esp_timer_get_time() + kRetryIntervalUs;
    }
    ESP_LOGW(kTag, "Settings acknowledgement not confirmed: result=%d current=%d", static_cast<int>(status), current);
}

void Application::PollWatchWakeListening() {
    if (watch_wake_retirement_) {
        if (!audio_service_.QuiesceAssistant(0)) return;
        audio_session_coordinator_.Fail(*watch_wake_retirement_, true);
        watch_wake_retirement_.reset();
    }
    const auto now = esp_timer_get_time();
    std::lock_guard<std::mutex> lock(watch_voice_mutex_);
    bool authorized = !WatchFirmwareBusy() && watch_wake_guard_ready_ && watch_always_available_ && WatchCloudPowerReady() &&
        watch_cloud_authorization_ && watch_cloud_authorization_->Usable(now / 1000) &&
        watch_cloud_authorization_->settings.SupportsWakeRuntime() && watch_wake_confirmed_revision_ &&
        watch_wake_confirmed_revision_ == watch_cloud_authorization_->settings.revision &&
        watch_wake_confirmed_intent_ == watch_voice_request_ &&
        watch_cloud_binding_generation_ == watch_voice::ActiveBindingGeneration() &&
        audio_service_.IsAssistantUserEnabled() && GetWatchWifiGate().Enabled() && GetWatchWifiGate().StationAllowed() &&
        WifiStation::GetInstance().IsConnected() && !GetWatchWifiGate().TransferActive() &&
        !watch_cloud_task_.Active() && !watch_cloud_application_ && !watch_cloud_ack_ &&
        !watch_cloud_retirement_ && !watch_power_change_ && !watch_deferred_wifi_request_ && !reset_protocol_pending_ &&
        protocol_ && !HasAssistantSession() && !assistant_session_.IsClosing() && !pending_assistant_listening_ &&
        !Board::GetInstance().GetWatchRecordingControlStatus().starting &&
        audio_session_coordinator_.Mode() == AudioSessionMode::kIdle;
    if (authorized && watch_wake_started_ && !watch_wake_lease_.Allows(watch_cloud_binding_generation_,
        audio_session_coordinator_.Generation(), now / 1000)) authorized = false;
    if (!authorized) {
        watch_wake_lease_.Revoke();
        if (watch_wake_started_) {
            if (audio_session_coordinator_.Mode() == AudioSessionMode::kIdle) {
                const auto reservation = audio_session_coordinator_.Begin(AudioSessionMode::kMaintenance);
                if (!reservation) return;
                watch_wake_retirement_ = reservation;
            }
            // A recording/session/sleep owner performs its own checked drain.
            watch_wake_started_ = false;
        }
        return;
    }
    if (watch_wake_started_ && audio_service_.IsWakeWordRunning()) return;
    if (now < watch_wake_retry_after_us_ || !audio_service_.IsIdle()) return;
    watch_wake_retry_after_us_ = now + kRetryIntervalUs;
    if (!audio_service_.ResumeAssistantAudio()) return;
    if (!watch_wake_lease_.Grant(watch_cloud_binding_generation_, audio_session_coordinator_.Generation(),
        now / 1000, watch_cloud_authorization_->expires_at_ms - 5000)) return;
    audio_service_.EnableWakeWordDetection(true);
    watch_wake_started_ = audio_service_.IsWakeWordRunning();
    if (!watch_wake_started_) {
        watch_wake_lease_.Revoke();
        const auto reservation = audio_session_coordinator_.Begin(AudioSessionMode::kMaintenance);
        if (reservation) watch_wake_retirement_ = reservation;
        ESP_LOGW(kTag, "Authorized wake-word detection could not start");
    } else {
        watch_wake_retry_after_us_ = 0;
        ESP_LOGI(kTag, "Authorized wake-word detection started");
    }
}
