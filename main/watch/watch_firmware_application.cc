#include "application.h"
#include "board.h"
#include "firmware_release_identity.h"
#include "assets.h"
#include "watch/watch_view_state.h"
#include "watch/watch_wifi_gate.h"
#include "wifi_station.h"
#include "system/audream_power_manager.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <algorithm>

namespace {
constexpr int64_t kModelValidationHealthyMs = 30000;
constexpr uint32_t kModelValidationCaptureMs = 35000;

bool FirmwareBatteryReady(bool installing) {
    int battery = -1;
    bool charging = false, discharging = false;
    return Board::GetInstance().GetBatteryLevel(battery, charging, discharging) &&
        (charging || battery >= (installing ? 20 : 5));
}
bool FirmwareConfigured() {
    const auto identity = firmware_release::CurrentIdentity();
    const std::string key = CONFIG_AUDREAM_OTA_PUBLIC_KEY_HEX;
    return identity && key.size() == 64 && std::all_of(key.begin(), key.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }) && !firmware_release::DiscoveryUrl(CONFIG_AUDREAM_OTA_API_ORIGIN, identity->model, identity->sequence).empty();
}
}

void Application::InitializeWatchFirmware(watch_update::Target target) {
    if (watch_firmware_) {
        const auto current = watch_firmware_->Read();
        if (current.target == target || current.active || current.commit_started || current.boot_selected ||
            current.phase == watch_update::Phase::kResourceFault || watch_firmware_preparation_) return;
        if (!watch_firmware_->StopWithin(0)) return;
    }
    watch_update::Controller::Hooks hooks;
    hooks.target = target;
    hooks.check = [this, target](const auto& cancelled) -> watch_update::CheckResult {
        const auto stop = [this, &cancelled] {
            return cancelled() || watch_firmware_revoked_.load() ||
                !audio_session_coordinator_.OwnsFirmwareUpdateResources(watch_firmware_generation_.load());
        };
        if (stop()) return firmware_release::CheckResult{firmware_release::CheckStatus::kCancelled, {}, {}};
        if (!PrepareWatchTlsClock())
            return firmware_release::CheckResult{firmware_release::CheckStatus::kTransportError, {}, {}};
        if (target == watch_update::Target::kModels) {
            if (!watch_asset_update_) return asset_update::CheckResult{};
            return watch_asset_update_->Check(CONFIG_AUDREAM_ASSET_MANIFEST_URL, stop);
        }
        auto result = Ota::CheckSignedRelease(stop);
        if (result.status == firmware_release::CheckStatus::kAvailable && result.release &&
            !Assets::GetInstance().SupportsFirmware(result.release->Sequence())) {
            ESP_LOGE("WatchFirmware", "Firmware release %u has no confirmed compatible model assets",
                static_cast<unsigned>(result.release->Sequence()));
            return firmware_release::CheckResult{firmware_release::CheckStatus::kInvalidRelease, {}, {}};
        }
        return result;
    };
    hooks.install = [this, target](const auto& envelope, const auto& progress, const auto& cancelled, const auto& commit) {
        const auto stop = [this, &cancelled] {
            return cancelled() || watch_firmware_revoked_.load() ||
                !audio_session_coordinator_.OwnsFirmwareUpdateResources(watch_firmware_generation_.load());
        };
        if (stop() || !PrepareWatchTlsClock()) return firmware_release::InstallResult{};
        if (target == watch_update::Target::kModels) {
            if (!watch_asset_update_) return firmware_release::InstallResult{};
            return firmware_release::InstallResult{
                watch_asset_update_->Install(envelope, progress, stop, commit) == asset_storage::StageResult::kStaged, true};
        }
        return Ota::InstallSignedEnvelope(envelope, progress, stop, commit);
    };
    hooks.acquire = [this](auto operation) { return AcquireWatchFirmware(operation); };
    hooks.release = [this](auto operation, bool safe) { return ReleaseWatchFirmware(operation, safe); };
    hooks.now_ms = [] { return esp_timer_get_time() / 1000; };
    hooks.pin_commit = [this] {
        return !watch_firmware_revoked_.load() &&
            audio_session_coordinator_.BeginFirmwareCommit(watch_firmware_generation_.load());
    };
    auto replacement = std::make_unique<watch_update::Controller>(std::move(hooks));
    std::lock_guard<std::mutex> lock(watch_firmware_intent_mutex_);
    watch_firmware_ = std::move(replacement);
    watch_firmware_request_ = 0;
    watch_firmware_preparation_phase_ = watch_update::Phase::kIdle;
    watch_firmware_busy_.store(false);
}

WatchUpdateBlock Application::WatchFirmwareBlock(bool installing) {
    auto& board = Board::GetInstance();
    auto& gate = GetWatchWifiGate();
    if (AudreamPowerManager::GetInstance().State() == AudreamPowerState::kDeepSleepPreparing ||
        audio_session_coordinator_.Mode() != AudioSessionMode::kIdle) return WatchUpdateBlock::kBusy;
    if (watch_firmware_ && watch_firmware_->Read().target == watch_update::Target::kModels) {
        if (!watch_assets_initialized_ || !firmware_release::IsHttpsUrl(CONFIG_AUDREAM_ASSET_MANIFEST_URL) ||
            !Assets::GetInstance().UpdatesReady()) return WatchUpdateBlock::kNotConfigured;
    } else if (!FirmwareConfigured()) return WatchUpdateBlock::kNotConfigured;
    if (!gate.Enabled()) return WatchUpdateBlock::kWifiDisabled;
    if (!board.HasWatchWifiCredentials()) return WatchUpdateBlock::kNoCredentials;
    if (!FirmwareBatteryReady(installing)) return WatchUpdateBlock::kBattery;
    if (gate.TransferActive() || board.IsWatchWifiProvisioning() || board.GetWatchRecordingControlStatus().starting ||
        (!audio_service_.IsIdle() && !audio_service_.IsWakeWordRunning()) ||
        HasAssistantSession() || pending_assistant_listening_ || assistant_session_.IsClosing() || reset_protocol_pending_ ||
        watch_cloud_task_.Active() || watch_cloud_application_ || watch_cloud_ack_ || watch_cloud_retirement_ ||
        watch_wake_retirement_ || watch_power_change_ || watch_deferred_wifi_request_ || watch_network_preparing_ ||
        watch_library_request_) return WatchUpdateBlock::kBusy;
    return WatchUpdateBlock::kNone;
}

bool Application::AcquireWatchFirmware(watch_update::Operation operation) {
    if (watch_firmware_revoked_.load() || !watch_firmware_network_ ||
        WatchFirmwareBlock(operation == watch_update::Operation::kInstall) != WatchUpdateBlock::kNone ||
        !GetWatchWifiGate().StationAllowed() || !WifiStation::GetInstance().IsConnected() ||
        !audio_service_.IsIdle() || !audio_service_.IsAssistantCaptureDrained() || audio_service_.IsWakeWordRunning()) return false;
    const auto lease = audio_session_coordinator_.Begin(AudioSessionMode::kFirmwareUpdate);
    if (!lease) return false;
    watch_firmware_generation_.store(lease.generation);
    watch_firmware_storage_ = AudreamPowerManager::GetInstance().BeginStorageRead();
    const bool models = watch_firmware_->Read().target == watch_update::Target::kModels;
    if (watch_firmware_storage_) {
        (void)Assets::GetInstance();
        watch_assets_initialized_ = true;
        if (models) watch_asset_update_ = Assets::GetInstance().BeginUpdate();
    }
    if (!watch_firmware_storage_ || (models && !watch_asset_update_) || watch_firmware_revoked_.load() ||
        !audio_session_coordinator_.Complete(lease)) {
        ReleaseWatchFirmware(operation, true);
        return false;
    }
    return true;
}

void Application::ReleaseWatchFirmwareNetwork() {
    if (watch_firmware_network_) {
        AudreamPowerManager::GetInstance().EndVoiceNetworkRequest();
        watch_firmware_network_ = false;
    }
    if (!watch_network_preparing_ && !HasAssistantSession() && !pending_assistant_listening_ && !watch_power_change_)
        watch_station_policy_.CancelDemand();
}

bool Application::ReleaseWatchFirmware(watch_update::Operation, bool io_safe) {
    const auto generation = watch_firmware_generation_.load();
    if (!io_safe) {
        audio_session_coordinator_.ReleaseFirmwareUpdate(generation, false);
        return false;
    }
    watch_asset_update_.reset();
    if (watch_firmware_storage_) {
        AudreamPowerManager::GetInstance().EndStorageRead();
        watch_firmware_storage_ = false;
    }
    ReleaseWatchFirmwareNetwork();
    const bool current = audio_session_coordinator_.IsCurrent(generation);
    const bool released = audio_session_coordinator_.ReleaseFirmwareUpdate(generation, true);
    // A recording handoff owns the newer generation and performs its own drain.
    if (!current || audio_session_coordinator_.Mode() != AudioSessionMode::kFirmwareCommit)
        watch_firmware_generation_.store(0);
    return !current || released;
}

void Application::ObserveWatchFirmwareIntent(const WatchViewRequest& request) {
    const bool cancel = request.action == WatchViewAction::kCancelFirmware;
    const bool wifi_off = request.action == WatchViewAction::kSetWifiEnabled && !request.enabled;
    if (!cancel && !wifi_off) return;
    std::lock_guard<std::mutex> lock(watch_firmware_intent_mutex_);
    if (cancel && (!watch_firmware_ || request.update_target != watch_firmware_->Read().target)) return;
    const auto cancelled = wifi_off ? request.id : request.firmware_request;
    watch_firmware_cancelled_request_ = std::max(watch_firmware_cancelled_request_, cancelled);
    if (watch_firmware_ && (wifi_off || (cancelled && cancelled == watch_firmware_request_))) {
        watch_firmware_revoked_.store(true);
        watch_firmware_->Cancel(watch_firmware_->Read().attempt);
    }
}

bool Application::HandleWatchFirmwareRequest(const WatchViewRequest& request) {
    using watch_update::Phase;
    if (!watch_firmware_ || !request.id) return false;
    const auto snapshot = watch_firmware_->Read();
    if (request.action == WatchViewAction::kValidateModels)
        return request.update_target == watch_update::Target::kModels && StartWatchModelValidation(request.id);
    if (request.action == WatchViewAction::kCancelModelValidation)
        return request.update_target == watch_update::Target::kModels &&
            StopWatchModelValidation(request.firmware_request, true, 3000);
    if (request.action == WatchViewAction::kRollbackModels) {
        const auto& assets = Assets::GetInstance();
        if (request.update_target != watch_update::Target::kModels || snapshot.target != watch_update::Target::kModels ||
            (assets.TrialState() != asset_storage::TrialStatus::kFailed &&
             !(assets.TrialState() == asset_storage::TrialStatus::kPending &&
               watch_model_validation_phase_ == WatchModelValidationPhase::kCleanupFailed)) ||
            watch_model_validation_generation_ ||
            snapshot.active || snapshot.commit_started || snapshot.boot_selected || watch_firmware_preparation_ ||
            audio_session_coordinator_.Mode() != AudioSessionMode::kIdle || !audio_service_.IsIdle() ||
            Board::GetInstance().GetWatchRecordingControlStatus().starting) return false;
        Reboot();
        return false;
    }
    if (request.action == WatchViewAction::kSelectUpdateTarget) {
        if ((request.update_target != watch_update::Target::kFirmware && request.update_target != watch_update::Target::kModels) ||
            snapshot.active || snapshot.commit_started || snapshot.boot_selected ||
            snapshot.phase == Phase::kResourceFault || watch_firmware_preparation_ ||
            audio_session_coordinator_.Mode() != AudioSessionMode::kIdle ||
            Board::GetInstance().GetWatchRecordingControlStatus().starting ||
            AudreamPowerManager::GetInstance().State() == AudreamPowerState::kDeepSleepPreparing) return false;
        if (request.update_target == watch_update::Target::kModels && !watch_assets_initialized_) {
            auto& power = AudreamPowerManager::GetInstance();
            if (!power.BeginStorageRead()) return false;
            (void)Assets::GetInstance();
            watch_assets_initialized_ = true;
            power.EndStorageRead();
        }
        InitializeWatchFirmware(request.update_target);
        return watch_firmware_->Read().target == request.update_target;
    }
    if (request.update_target != snapshot.target) return false;
    if (request.action == WatchViewAction::kCancelFirmware) {
        {
            std::lock_guard<std::mutex> lock(watch_firmware_intent_mutex_);
            if (!request.firmware_request || request.firmware_request != watch_firmware_request_) return false;
        }
        if (snapshot.commit_started || snapshot.boot_selected || snapshot.phase == Phase::kResourceFault) return false;
        StopWatchFirmware(0);
        return true;
    }
    if (request.action == WatchViewAction::kRebootFirmware) {
        {
            std::lock_guard<std::mutex> lock(watch_firmware_intent_mutex_);
            if (request.firmware_request != watch_firmware_request_ || !request.firmware_request) return false;
        }
        if (!snapshot.boot_selected || snapshot.active || snapshot.phase != Phase::kRebootRequired ||
            request.firmware_attempt != snapshot.attempt || request.firmware_sequence != snapshot.sequence ||
            audio_session_coordinator_.Mode() != AudioSessionMode::kFirmwareCommit ||
            !audio_session_coordinator_.IsCurrent(watch_firmware_generation_.load()) ||
            Board::GetInstance().GetWatchRecordingControlStatus().starting ||
            !audio_service_.Stop() || (protocol_ && !protocol_->CloseAudioChannelWithin(0))) return false;
        Reboot();
        return false;  // Successful hardware restart does not return.
    }
    const bool installing = request.action == WatchViewAction::kInstallFirmware;
    if ((!installing && request.action != WatchViewAction::kCheckFirmware) || watch_firmware_preparation_ ||
        snapshot.active || snapshot.commit_started || snapshot.boot_selected || snapshot.phase == Phase::kResourceFault ||
        WatchFirmwareBlock(installing) != WatchUpdateBlock::kNone) return false;
    if (installing ? snapshot.phase != Phase::kAvailable || request.firmware_attempt != snapshot.attempt ||
                    request.firmware_sequence != snapshot.sequence : snapshot.phase == Phase::kAvailable) return false;
    {
        std::lock_guard<std::mutex> lock(watch_firmware_intent_mutex_);
        const auto root = installing ? request.firmware_request : request.id;
        if (!root || root <= watch_firmware_cancelled_request_ || (installing && root != watch_firmware_request_)) return false;
        watch_firmware_request_ = root;
        watch_firmware_revoked_.store(false);
    }
    if (!AudreamPowerManager::GetInstance().BeginVoiceNetworkRequest()) return false;
    watch_firmware_network_ = true;
    watch_firmware_preparation_ = std::make_unique<WatchViewRequest>(request);
    watch_firmware_preparation_phase_ = Phase::kIdle;
    watch_firmware_deadline_us_ = esp_timer_get_time() + 90000000;
    watch_firmware_busy_.store(true);
    watch_wake_lease_.Revoke();
    watch_station_policy_.Request(esp_timer_get_time() / 1000);
    GetWatchWifiGate().SetStationAllowed(true);
    return true;
}

bool Application::StopWatchFirmware(uint32_t timeout_ms) {
    if (watch_model_validation_generation_ && !StopWatchModelValidation(0, true, timeout_ms)) return false;
    watch_firmware_revoked_.store(true);
    if (watch_firmware_preparation_) {
        watch_firmware_preparation_.reset();
        watch_firmware_preparation_phase_ = watch_update::Phase::kCancelled;
        ReleaseWatchFirmwareNetwork();
    }
    const bool safe = !watch_firmware_ || watch_firmware_->StopWithin(timeout_ms);
    watch_firmware_busy_.store(!safe);
    return safe;
}

void Application::PollWatchFirmware() {
    PollWatchModelValidation();
    if (!watch_firmware_) return;
    watch_firmware_->Poll();
    auto raw = watch_firmware_->Read();
    if (watch_firmware_revoked_.load() ||
        (raw.active && !raw.commit_started && !FirmwareBatteryReady(raw.phase == watch_update::Phase::kInstalling)) ||
        (raw.active && (!GetWatchWifiGate().Enabled() || !GetWatchWifiGate().StationAllowed() ||
            !WifiStation::GetInstance().IsConnected() || GetWatchWifiGate().TransferActive() ||
            !audio_session_coordinator_.OwnsFirmwareUpdateResources(watch_firmware_generation_.load())))) {
        StopWatchFirmware(0);
        return;
    }
    if (!watch_firmware_preparation_) {
        watch_firmware_busy_.store(raw.active || raw.commit_started || raw.phase == watch_update::Phase::kResourceFault);
        return;
    }
    const bool installing = watch_firmware_preparation_->action == WatchViewAction::kInstallFirmware;
    const auto mode = audio_session_coordinator_.Mode();
    if (!GetWatchWifiGate().Enabled() || GetWatchWifiGate().TransferActive() ||
        Board::GetInstance().GetWatchRecordingControlStatus().starting ||
        (mode != AudioSessionMode::kIdle && !watch_wake_retirement_)) {
        StopWatchFirmware(0);
        return;
    }
    const auto now = esp_timer_get_time();
    if (now >= watch_firmware_deadline_us_ || now < watch_firmware_deadline_us_ - 90000000 || watch_station_policy_.Failed()) {
        StopWatchFirmware(0);
        watch_firmware_preparation_phase_ = installing ? watch_update::Phase::kInstallFailed : watch_update::Phase::kCheckFailed;
        return;
    }
    if (!GetWatchWifiGate().StationAllowed() || !WifiStation::GetInstance().IsConnected() || watch_wake_retirement_ ||
        audio_service_.IsWakeWordRunning() || !audio_service_.IsIdle() || !audio_service_.IsAssistantCaptureDrained()) return;
    if (protocol_ && !protocol_->CloseAudioChannelWithin(0)) return;
    const auto request = *watch_firmware_preparation_;
    const bool accepted = installing ? watch_firmware_->Confirm(request.firmware_attempt, request.firmware_sequence)
                                    : watch_firmware_->Check();
    watch_firmware_preparation_.reset();
    if (!accepted) {
        ReleaseWatchFirmwareNetwork();
        raw = watch_firmware_->Read();
        if (raw.phase == watch_update::Phase::kAvailable)
            watch_firmware_preparation_phase_ = watch_update::Phase::kInstallFailed;
        watch_firmware_busy_.store(raw.commit_started || raw.phase == watch_update::Phase::kResourceFault);
    }
}

WatchFirmwareSnapshot Application::BuildWatchFirmwareSnapshot() {
    WatchFirmwareSnapshot state;
    if (!watch_firmware_) return state;
    state.available = true;
    state.update = watch_firmware_->Read();
    const auto& assets = Assets::GetInstance();
    state.model_trial = assets.TrialState();
    state.model_validation_active = watch_model_validation_generation_ != 0;
    state.model_validation_request = watch_model_validation_request_;
    state.model_validation_phase = watch_model_validation_phase_;
    if (state.model_validation_active) {
        const auto elapsed = std::max<int64_t>(0, esp_timer_get_time() / 1000 - watch_model_validation_started_ms_);
        state.model_validation_progress = static_cast<unsigned>(std::min<int64_t>(99,
            elapsed * 100 / kModelValidationHealthyMs));
    }
    if (state.update.target == watch_update::Target::kFirmware)
        state.current_version = esp_app_get_description()->version;
    else if (watch_assets_initialized_) {
        state.current_version = assets.CurrentVersion();
        state.current_builtin = assets.partition_valid() && state.current_version.empty();
    }
    if (watch_firmware_preparation_phase_ != watch_update::Phase::kIdle) state.update.phase = watch_firmware_preparation_phase_;
    state.network_preparing = watch_firmware_preparation_ != nullptr;
    {
        std::lock_guard<std::mutex> lock(watch_firmware_intent_mutex_);
        state.request = watch_firmware_request_;
    }
    const bool installing = state.update.phase == watch_update::Phase::kAvailable;
    state.blocked = WatchFirmwareBlock(installing);
    const bool usable = state.blocked == WatchUpdateBlock::kNone && !state.network_preparing &&
        !state.update.active && !state.update.commit_started && !state.update.boot_selected &&
        state.update.phase != watch_update::Phase::kResourceFault;
    state.can_check = usable && !installing;
    state.can_install = usable && installing;
    state.can_reboot = state.update.phase == watch_update::Phase::kRebootRequired && state.update.boot_selected &&
        !state.update.active && audio_session_coordinator_.Mode() == AudioSessionMode::kFirmwareCommit &&
        audio_session_coordinator_.IsCurrent(watch_firmware_generation_.load());
    const bool model_idle = state.update.target == watch_update::Target::kModels && !state.update.active &&
        !state.update.commit_started && !state.update.boot_selected && !state.network_preparing &&
        audio_session_coordinator_.Mode() == AudioSessionMode::kIdle && audio_service_.IsIdle() &&
        !Board::GetInstance().GetWatchRecordingControlStatus().starting;
    state.can_validate_models = model_idle && state.model_trial == asset_storage::TrialStatus::kPending &&
        assets.Applied() && watch_boot_health_.Confirmed() && !watch_model_validation_generation_;
    state.can_rollback_models = model_idle && !watch_model_validation_generation_ &&
        (state.model_trial == asset_storage::TrialStatus::kFailed ||
         (state.model_trial == asset_storage::TrialStatus::kPending &&
          state.model_validation_phase == WatchModelValidationPhase::kCleanupFailed));
    return state;
}

bool Application::StartWatchModelValidation(uint64_t request_id) {
    if (!request_id || !watch_firmware_ || watch_model_validation_generation_ ||
        watch_firmware_->Read().target != watch_update::Target::kModels ||
        watch_firmware_->Read().active || watch_firmware_->Read().commit_started ||
        watch_firmware_->Read().boot_selected || watch_firmware_preparation_ ||
        audio_session_coordinator_.Mode() != AudioSessionMode::kIdle || !audio_service_.IsIdle() ||
        Board::GetInstance().GetWatchRecordingControlStatus().starting || watch_cloud_task_.Active() ||
        HasAssistantSession() || pending_assistant_listening_ || assistant_session_.IsClosing() || reset_protocol_pending_ ||
        !watch_boot_health_.Confirmed()) return false;
    auto& assets = Assets::GetInstance();
    if (!assets.Applied() || assets.TrialState() != asset_storage::TrialStatus::kPending) return false;
    const auto transition = audio_session_coordinator_.Begin(AudioSessionMode::kModelValidation);
    if (!transition) return false;
    watch_wake_lease_.Revoke();
    bool ready = audio_service_.QuiesceAssistant(3000);
    bool health_started = false;
    if (ready) {
        watch_model_validation_storage_ = AudreamPowerManager::GetInstance().BeginStorageRead();
        ready = watch_model_validation_storage_;
    }
    srmodel_list_t* models = nullptr;
    if (ready) ready = assets.LoadAssistantModels(models) && audio_service_.PrepareWatchWakeWord(models) &&
        audio_service_.PrepareVoiceProcessing(models, true);
    if (ready) {
        health_started = assets.BeginTrialHealth(true, esp_timer_get_time() / 1000,
            audio_service_.ModelValidationFeedCount());
        ready = health_started;
    }
    if (ready) ready = audio_session_coordinator_.Complete(transition) &&
        audio_service_.StartModelValidation(transition.generation, kModelValidationCaptureMs);
    if (!ready) {
        if (health_started) assets.EndTrialHealth(true);
        audio_service_.RevokeModelValidation(transition.generation);
        const bool drained = audio_service_.QuiesceAssistant(3000);
        if (watch_model_validation_storage_) {
            AudreamPowerManager::GetInstance().EndStorageRead();
            watch_model_validation_storage_ = false;
        }
        const bool released = audio_session_coordinator_.OwnsModelValidationResources(transition.generation)
            ? audio_session_coordinator_.ReleaseModelValidation(transition.generation, drained)
            : audio_session_coordinator_.Fail(transition, drained);
        if (released) audio_service_.ResumeAssistantAudio();
        watch_model_validation_phase_ = !released ? WatchModelValidationPhase::kCleanupFailed
            : health_started ? WatchModelValidationPhase::kFailed : WatchModelValidationPhase::kIdle;
        return false;
    }
    watch_model_validation_request_ = request_id;
    watch_model_validation_generation_ = transition.generation;
    watch_model_validation_started_ms_ = esp_timer_get_time() / 1000;
    watch_model_validation_phase_ = WatchModelValidationPhase::kRunning;
    watch_firmware_busy_.store(true);
    ESP_LOGI("WatchFirmware", "Local model validation started");
    return true;
}

bool Application::StopWatchModelValidation(uint64_t request_id, bool cancelled, uint32_t timeout_ms) {
    const auto generation = watch_model_validation_generation_;
    if (!generation || (request_id && request_id != watch_model_validation_request_)) return false;
    audio_service_.RevokeModelValidation(generation);
    Assets::GetInstance().EndTrialHealth(!cancelled);
    const bool drained = audio_service_.QuiesceAssistant(timeout_ms);
    const bool current = audio_session_coordinator_.OwnsModelValidationResources(generation);
    const bool released = !current || audio_session_coordinator_.ReleaseModelValidation(generation, drained);
    if (watch_model_validation_storage_) {
        AudreamPowerManager::GetInstance().EndStorageRead();
        watch_model_validation_storage_ = false;
    }
    watch_model_validation_request_ = 0;
    watch_model_validation_generation_ = 0;
    watch_model_validation_started_ms_ = 0;
    watch_model_validation_phase_ = !drained || !released ? WatchModelValidationPhase::kCleanupFailed
        : cancelled ? WatchModelValidationPhase::kCancelled : WatchModelValidationPhase::kFailed;
    if (released && audio_session_coordinator_.Mode() == AudioSessionMode::kIdle) audio_service_.ResumeAssistantAudio();
    const auto update = watch_firmware_ ? watch_firmware_->Read() : watch_update::Snapshot{};
    watch_firmware_busy_.store(update.active || update.commit_started || update.phase == watch_update::Phase::kResourceFault);
    return drained && released;
}

void Application::PollWatchModelValidation() {
    const auto generation = watch_model_validation_generation_;
    if (!generation) return;
    if (!audio_session_coordinator_.OwnsModelValidationResources(generation)) {
        StopWatchModelValidation(0, true, 3000);
        return;
    }
    const auto now_ms = esp_timer_get_time() / 1000;
    const auto feeds = audio_service_.ModelValidationFeedCount();
    const bool current = audio_service_.IsModelValidationCurrent(generation);
    const bool active = Assets::GetInstance().PollTrialHealth(now_ms,
        current && feeds > 0 && watch_boot_health_.Confirmed(), feeds);
    const auto trial = Assets::GetInstance().TrialState();
    if (!active && trial == asset_storage::TrialStatus::kNone) {
        audio_service_.RevokeModelValidation(generation);
        const bool drained = audio_service_.QuiesceAssistant(3000);
        const bool released = audio_session_coordinator_.ReleaseModelValidation(generation, drained);
        if (watch_model_validation_storage_) {
            AudreamPowerManager::GetInstance().EndStorageRead();
            watch_model_validation_storage_ = false;
        }
        watch_model_validation_request_ = 0;
        watch_model_validation_generation_ = 0;
        watch_model_validation_started_ms_ = 0;
        watch_model_validation_phase_ = drained && released ? WatchModelValidationPhase::kSucceeded
            : WatchModelValidationPhase::kCleanupFailed;
        if (released) audio_service_.ResumeAssistantAudio();
        watch_firmware_busy_.store(!released);
        ESP_LOGI("WatchFirmware", "Local model validation completed: success=%d", drained && released);
        return;
    }
    if (!current || trial == asset_storage::TrialStatus::kFailed ||
        now_ms - watch_model_validation_started_ms_ >= kModelValidationCaptureMs) {
        StopWatchModelValidation(0, false, 3000);
    }
}
