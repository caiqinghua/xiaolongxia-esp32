#include "application.h"
#include "board.h"
#include "esp_timer.h"
#include "system/audream_power_manager.h"
#include "watch/watch_wifi_gate.h"
#include "firmware_release_fixture.h"
#include "asset_release_fixture.h"
#include <chrono>
#include <iostream>
#include <thread>

using watch_update::Phase;
using firmware_release::CheckStatus;
namespace {
std::atomic<bool> blocked{false}, proceed{false};
std::atomic<int> checks{0}, installs{0};
int block_stage = 0;
bool tls_ready = true, io_safe = true, install_ok = true;
firmware_release::CheckResult result;
asset_update::CheckResult model_result;
template<class F> void Wait(F predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate()) { assert(std::chrono::steady_clock::now() < deadline); std::this_thread::yield(); }
}
void Barrier(int stage) {
    if (block_stage != stage) return;
    blocked = true;
    Wait([] { return proceed.load(); });
}
void Reset() {
    auto& power = AudreamPowerManager::GetInstance();
    assert(!power.network_leases && !power.storage_leases);
    auto& assets = Assets::GetInstance();
    assert(!assets.update_owned);
    assets.updates_ready = assets.update_acquire_ok = true;
    assets.applied = true;
    assets.trial_state = asset_storage::TrialStatus::kNone;
    assets.keep_observing = true;
    assets.update_acquires = assets.update_returns = 0;
    power.state = AudreamPowerState::kActive; power.allowed = true;
    GetWatchWifiGate().Initialize(true);
    GetWatchWifiGate().SetStationAllowed(true);
    WifiStation::GetInstance().connected = true;
    Board::GetInstance() = {};
    test_voice_now_us = 10000000;
    blocked = proceed = false; checks = installs = 0; block_stage = 0;
    tls_ready = io_safe = install_ok = true;
    result = {};
    result.status = CheckStatus::kAvailable;
    result.envelope = release_fixture::Seal(release_fixture::Payload(std::string(5000, 'x')));
    result.release = firmware_release::Verify(result.envelope, "test-watch", 7, release_fixture::PublicKey());
    assert(result.release);
    model_result = {CheckStatus::kAvailable, {}, asset_fixture::Sign(asset_fixture::Payload(asset_fixture::Bundle()))};
    model_result.release = asset_fixture::Verify(model_result.envelope);
    assert(model_result.release);
    assets.update_check = [](const std::string& url, const auto& cancelled) {
        assert(url == "https://updates.example/models/latest" && Assets::GetInstance().update_owned);
        assert(AudreamPowerManager::GetInstance().storage_leases == 1);
        ++checks; Barrier(1);
        if (cancelled()) return asset_update::CheckResult{CheckStatus::kCancelled, {}, {}};
        return model_result;
    };
    assets.update_install = [](const std::string& envelope, const auto& progress, const auto& cancelled, const auto& commit) {
        assert(envelope == model_result.envelope && Assets::GetInstance().update_owned);
        assert(AudreamPowerManager::GetInstance().storage_leases == 1);
        ++installs; progress(55, 1024); Barrier(2);
        if (cancelled() || !commit()) return asset_storage::StageResult::kCancelled;
        Barrier(3);
        Assets::GetInstance().updates_ready = false;
        return install_ok ? asset_storage::StageResult::kStaged : asset_storage::StageResult::kPersistenceUncertain;
    };
}
WatchViewRequest CheckRequest(uint64_t id = 1) {
    WatchViewRequest r; r.id = id; r.action = WatchViewAction::kCheckFirmware; return r;
}
WatchViewRequest CancelRequest(uint64_t id = 2, uint64_t root = 1) {
    WatchViewRequest r; r.id = id; r.action = WatchViewAction::kCancelFirmware; r.firmware_request = root; return r;
}
void Finish(Application& app) {
    Wait([&] { app.PollWatchFirmware(); return !app.watch_firmware_->Read().active && !app.watch_firmware_preparation_; });
}
void Available(Application& app) {
    app.InitializeWatchFirmware();
    assert(app.HandleWatchFirmwareRequest(CheckRequest()));
    Finish(app);
    assert(app.BuildWatchFirmwareSnapshot().can_install);
}
void ConfirmBoot(Application& app) {
    app.watch_boot_health_.Begin(true, 0);
    for (int second = 1; second <= 30; ++second) {
        const bool confirming = app.watch_boot_health_.Observe(second * 1000, true);
        if (second < 30) assert(!confirming);
        else assert(confirming);
    }
    app.watch_boot_health_.Complete(true);
    assert(app.watch_boot_health_.Confirmed());
}
void SelectModels(Application& app, uint64_t id = 1) {
    app.InitializeWatchFirmware();
    WatchViewRequest select;
    select.id = id;
    select.action = WatchViewAction::kSelectUpdateTarget;
    select.update_target = watch_update::Target::kModels;
    assert(app.HandleWatchFirmwareRequest(select));
}
WatchViewRequest InstallRequest(Application& app) {
    const auto state = app.BuildWatchFirmwareSnapshot();
    WatchViewRequest r; r.id = 3; r.action = WatchViewAction::kInstallFirmware;
    r.firmware_request = state.request; r.firmware_attempt = state.update.attempt; r.firmware_sequence = state.update.sequence;
    r.update_target = state.update.target;
    return r;
}
}

bool Application::PrepareWatchTlsClock() { return tls_ready; }
bool Application::WatchCloudPowerReady() const { return AudreamPowerManager::GetInstance().State() == AudreamPowerState::kActive; }
namespace watch_voice { uint64_t ActiveBindingGeneration() { return 0; } }
firmware_release::CheckResult Ota::CheckSignedRelease(const watch_update::Controller::Cancelled& cancelled) {
    ++checks; Barrier(1);
    if (cancelled()) return {CheckStatus::kCancelled, {}, {}};
    return result;
}
firmware_release::InstallResult Ota::InstallSignedEnvelope(const std::string& envelope,
    const watch_update::Controller::Progress& progress, const watch_update::Controller::Cancelled& cancelled,
    const watch_update::Controller::Cancelled& commit) {
    ++installs; assert(envelope == result.envelope);
    progress(55, 1024); Barrier(2);
    if (cancelled() || !commit()) return {false, io_safe};
    Barrier(3);
    return {install_ok, io_safe};
}

int main() {
    assert(sodium_init() >= 0);
    {
        Reset(); Application app;
        app.audio_service_.enabled = false;
        Available(app);
        assert(checks == 1 && installs == 0 && !app.WatchFirmwareBusy());
        assert(app.BuildWatchFirmwareSnapshot().current_version == "2.0.0");
        auto request = InstallRequest(app);
        ++request.firmware_sequence;
        assert(!app.HandleWatchFirmwareRequest(request));
        --request.firmware_sequence;
        assert(app.HandleWatchFirmwareRequest(request)); Finish(app);
        auto state = app.BuildWatchFirmwareSnapshot();
        assert(state.can_reboot && state.update.progress == 100 && installs == 1 && app.WatchFirmwareBusy());
        request.id = 4; request.action = WatchViewAction::kRebootFirmware;
        assert(!app.HandleWatchFirmwareRequest(request) && app.reboots == 1);
        assert(!app.StopWatchFirmware(0));
    }
    {
        Reset(); Application app; app.InitializeWatchFirmware();
        auto cancel = CancelRequest(); app.ObserveWatchFirmwareIntent(cancel);
        assert(!app.HandleWatchFirmwareRequest(CheckRequest()) && !checks);
        assert(app.HandleWatchFirmwareRequest(CheckRequest(3)));
        app.ObserveWatchFirmwareIntent(cancel);
        assert(!app.HandleWatchFirmwareRequest(cancel));
        Finish(app); assert(checks == 1 && app.watch_firmware_->Read().phase == Phase::kAvailable);
    }
    for (int cause : {0, 1, 2, 3}) {
        Reset(); Application app; app.InitializeWatchFirmware();
        WifiStation::GetInstance().connected = false;
        assert(app.HandleWatchFirmwareRequest(CheckRequest()));
        app.PollWatchFirmware(); app.PollWatchNetwork();
        assert(!checks && Board::GetInstance().station_starts == 1 && app.BuildWatchFirmwareSnapshot().network_preparing);
        if (cause == 0) app.ObserveWatchFirmwareIntent(CancelRequest());
        if (cause == 1) assert(app.audio_session_coordinator_.Begin(AudioSessionMode::kLocalRecording));
        if (cause == 2) test_voice_now_us += 90000000;
        if (cause == 3) {
            WatchViewRequest off; off.id = 2; off.action = WatchViewAction::kSetWifiEnabled; off.enabled = false;
            app.ObserveWatchFirmwareIntent(off);
        }
        app.PollWatchFirmware();
        assert(!checks && !app.WatchFirmwareBusy() && !app.watch_firmware_network_);
        assert(app.BuildWatchFirmwareSnapshot().update.phase == (cause == 2 ? Phase::kCheckFailed : Phase::kCancelled));
    }
    {
        Reset(); Application app; app.audio_service_.enabled = false; app.InitializeWatchFirmware();
        WifiStation::GetInstance().connected = false;
        assert(app.HandleWatchFirmwareRequest(CheckRequest()));
        AudreamPowerManager::GetInstance().state = AudreamPowerState::kIdle;
        app.PollWatchNetwork(); assert(Board::GetInstance().station_starts == 1);
        WifiStation::GetInstance().connected = true;
        Finish(app); assert(checks == 1);
        app.PollWatchNetwork(); assert(Board::GetInstance().station_stops == 1);
    }
    for (int stage : {1, 2, 3}) {
        Reset(); Application app; app.InitializeWatchFirmware();
        if (stage != 1) Available(app);
        block_stage = stage;
        assert(app.HandleWatchFirmwareRequest(stage == 1 ? CheckRequest() : InstallRequest(app)));
        app.PollWatchFirmware(); Wait([] { return blocked.load(); });
        const auto recording = app.audio_session_coordinator_.Begin(AudioSessionMode::kLocalRecording);
        assert(static_cast<bool>(recording) == (stage != 3));
        assert(!app.StopWatchFirmware(0));
        assert(app.watch_firmware_network_ && app.watch_firmware_storage_);
        proceed = true; Finish(app);
        assert(!app.watch_firmware_network_ && !app.watch_firmware_storage_);
        if (stage != 3) {
            assert(app.StopWatchFirmware(0) && app.audio_session_coordinator_.Complete(recording));
            assert(!app.watch_firmware_->Read().boot_selected);
        } else assert(app.BuildWatchFirmwareSnapshot().can_reboot && !app.StopWatchFirmware(0));
    }
    for (int cause : {0, 1, 2, 3, 4}) {
        Reset(); Application app; app.InitializeWatchFirmware();
        if (cause == 0) Board::GetInstance().credentials = false;
        if (cause == 1) GetWatchWifiGate().Initialize(false);
        if (cause == 2) Board::GetInstance().battery = 4;
        if (cause == 3) AudreamPowerManager::GetInstance().state = AudreamPowerState::kDeepSleepPreparing;
        if (cause == 4) assert(app.audio_session_coordinator_.Begin(AudioSessionMode::kFileTransfer));
        assert(!app.BuildWatchFirmwareSnapshot().can_check && !app.HandleWatchFirmwareRequest(CheckRequest()) && !checks);
    }
    {
        Reset(); Application app; Available(app);
        Board::GetInstance().battery = 19;
        assert(!app.HandleWatchFirmwareRequest(InstallRequest(app)) && !installs);
        Board::GetInstance().charging = true;
        assert(app.HandleWatchFirmwareRequest(InstallRequest(app))); Finish(app);
        assert(app.BuildWatchFirmwareSnapshot().can_reboot);
    }
    {
        Reset(); Application app; app.InitializeWatchFirmware(); tls_ready = false;
        assert(app.HandleWatchFirmwareRequest(CheckRequest())); Finish(app);
        assert(!checks && app.watch_firmware_->Read().phase == Phase::kCheckFailed && !app.WatchFirmwareBusy());
    }
    for (bool low_battery : {true, false}) {
        Reset(); Application app; Available(app);
        block_stage = 2;
        assert(app.HandleWatchFirmwareRequest(InstallRequest(app)));
        app.PollWatchFirmware(); Wait([] { return blocked.load(); });
        if (low_battery) Board::GetInstance().battery = 10;
        else WifiStation::GetInstance().connected = false;
        app.PollWatchFirmware();
        assert(app.watch_firmware_->Read().cancelling && app.watch_firmware_storage_);
        proceed = true; Finish(app);
        assert(app.watch_firmware_->Read().phase == Phase::kCancelled && !app.watch_firmware_storage_);
    }
    {
        Reset(); Application app; Available(app);
        test_voice_now_us += 60000000;
        WifiStation::GetInstance().connected = false;
        assert(app.HandleWatchFirmwareRequest(InstallRequest(app)));
        app.PollWatchFirmware(); app.PollWatchNetwork();
        test_voice_now_us += 60000000;
        WifiStation::GetInstance().connected = true;
        Finish(app);
        assert(checks == 1 && installs == 0 && app.BuildWatchFirmwareSnapshot().update.phase == Phase::kExpired);
    }
    {
        Reset(); Application app; Available(app);
        assert(app.HandleWatchFirmwareRequest(InstallRequest(app)));
        io_safe = false; block_stage = 2;
        app.PollWatchFirmware(); Wait([] { return blocked.load(); });
        assert(!app.StopWatchFirmware(0)); proceed = true; Finish(app);
        assert(!app.StopWatchFirmware(0) && app.watch_firmware_->Read().phase == Phase::kResourceFault);
        assert(app.watch_firmware_network_ && app.watch_firmware_storage_ && !app.BuildWatchFirmwareSnapshot().can_check);
        // Simulate explicit teardown verification solely to release test leases.
        assert(app.ReleaseWatchFirmware(watch_update::Operation::kInstall, true));
    }
    for (int mode = 0; mode < 8; ++mode) {
        Reset(); Application app; app.InitializeWatchFirmware();
        auto& assets = Assets::GetInstance();
        WatchViewRequest select;
        select.id = 1; select.action = WatchViewAction::kSelectUpdateTarget;
        select.update_target = watch_update::Target::kModels;
        if (mode == 7) {
            assert(app.audio_session_coordinator_.Begin(AudioSessionMode::kLocalRecording));
            assert(!app.HandleWatchFirmwareRequest(select) && !app.watch_assets_initialized_);
            continue;
        }
        assert(app.HandleWatchFirmwareRequest(select) && app.watch_assets_initialized_ && !checks);
        auto check = CheckRequest(2);
        assert(!app.HandleWatchFirmwareRequest(check));
        check.update_target = watch_update::Target::kModels;
        if (mode == 1) assets.update_acquire_ok = false;
        if (mode == 2) block_stage = 1;
        assert(app.HandleWatchFirmwareRequest(check));
        if (mode == 2) {
            app.PollWatchFirmware(); Wait([] { return blocked.load(); });
            auto wrong_cancel = CancelRequest(3, 2);
            app.ObserveWatchFirmwareIntent(wrong_cancel);
            assert(!app.HandleWatchFirmwareRequest(wrong_cancel) && !app.watch_firmware_revoked_.load());
            select.update_target = watch_update::Target::kFirmware;
            assert(!app.HandleWatchFirmwareRequest(select));
            auto cancel = CancelRequest(3, 2); cancel.update_target = watch_update::Target::kModels;
            app.ObserveWatchFirmwareIntent(cancel);
            assert(app.HandleWatchFirmwareRequest(cancel));
            assert(assets.update_owned && app.watch_firmware_storage_ && app.WatchFirmwareBusy());
            proceed = true; Finish(app);
            assert(!assets.update_owned && assets.update_acquires == assets.update_returns);
            assert(app.watch_firmware_->Read().phase == Phase::kCancelled);
            continue;
        }
        Finish(app);
        if (mode == 1) {
            assert(app.watch_firmware_->Read().phase == Phase::kCheckFailed && !checks);
            assert(!assets.update_owned && !app.watch_firmware_network_ && !app.watch_firmware_storage_);
            continue;
        }
        assert(app.BuildWatchFirmwareSnapshot().can_install && !assets.update_owned && assets.update_returns == 1);
        auto install = InstallRequest(app);
        if (mode == 6) {
            select.id = 4; select.update_target = watch_update::Target::kFirmware;
            assert(app.HandleWatchFirmwareRequest(select));
            assert(!app.HandleWatchFirmwareRequest(install) && !installs);
            assert(app.BuildWatchFirmwareSnapshot().update.target == watch_update::Target::kFirmware);
            continue;
        }
        if (mode == 3) block_stage = 2;
        if (mode == 4) block_stage = 3;
        if (mode == 5) install_ok = false;
        assert(app.HandleWatchFirmwareRequest(install));
        if (mode == 3 || mode == 4) {
            app.PollWatchFirmware(); Wait([] { return blocked.load(); });
            if (mode == 3) assert(app.audio_session_coordinator_.Begin(AudioSessionMode::kLocalRecording));
            assert(!app.StopWatchFirmware(0) && assets.update_owned && app.watch_firmware_storage_);
            proceed = true;
        }
        Finish(app);
        assert(!assets.update_owned && assets.update_returns == 2 && !app.watch_firmware_storage_);
        if (mode == 3) assert(app.watch_firmware_->Read().phase == Phase::kCancelled);
        else if (mode == 5) assert(app.watch_firmware_->Read().phase == Phase::kCommitUncertain);
        else {
            const auto state = app.BuildWatchFirmwareSnapshot();
            assert(state.can_reboot && state.update.target == watch_update::Target::kModels);
            install.id = 4; install.action = WatchViewAction::kRebootFirmware;
            assert(!app.HandleWatchFirmwareRequest(install) && app.reboots == 1);
        }
    }
    {
        Reset(); Application app; SelectModels(app); ConfirmBoot(app);
        auto& assets = Assets::GetInstance();
        assets.trial_state = asset_storage::TrialStatus::kPending;
        WatchViewRequest validate;
        validate.id = 2; validate.action = WatchViewAction::kValidateModels;
        validate.update_target = watch_update::Target::kModels;
        assert(app.HandleWatchFirmwareRequest(validate));
        auto state = app.BuildWatchFirmwareSnapshot();
        assert(state.model_validation_active && state.model_validation_request == validate.id &&
            state.model_trial == asset_storage::TrialStatus::kObserving && app.WatchFirmwareBusy());
        assert(AudreamPowerManager::GetInstance().storage_leases == 1 &&
            !AudreamPowerManager::GetInstance().network_leases &&
            app.audio_session_coordinator_.Mode() == AudioSessionMode::kModelValidation &&
            app.audio_service_.IsAssistantUserEnabled());
        assets.keep_observing = false;
        app.PollWatchFirmware();
        state = app.BuildWatchFirmwareSnapshot();
        assert(!state.model_validation_active && state.model_validation_phase == WatchModelValidationPhase::kSucceeded &&
            state.model_trial == asset_storage::TrialStatus::kNone && !app.WatchFirmwareBusy() &&
            AudreamPowerManager::GetInstance().storage_leases == 0 &&
            app.audio_session_coordinator_.Mode() == AudioSessionMode::kIdle &&
            app.audio_service_.IsAssistantUserEnabled());
    }
    {
        Reset(); Application app; SelectModels(app); ConfirmBoot(app);
        auto& assets = Assets::GetInstance();
        assets.trial_state = asset_storage::TrialStatus::kPending;
        WatchViewRequest validate;
        validate.id = 2; validate.action = WatchViewAction::kValidateModels;
        validate.update_target = watch_update::Target::kModels;
        assert(app.HandleWatchFirmwareRequest(validate));
        WatchViewRequest cancel;
        cancel.id = 3; cancel.action = WatchViewAction::kCancelModelValidation;
        cancel.update_target = watch_update::Target::kModels; cancel.firmware_request = 99;
        assert(!app.HandleWatchFirmwareRequest(cancel));
        cancel.firmware_request = validate.id;
        assert(app.HandleWatchFirmwareRequest(cancel));
        auto state = app.BuildWatchFirmwareSnapshot();
        assert(!state.model_validation_active && state.model_trial == asset_storage::TrialStatus::kPending &&
            state.model_validation_phase == WatchModelValidationPhase::kCancelled && state.can_validate_models);
        validate.id = 4;
        assert(app.HandleWatchFirmwareRequest(validate));
        const auto recording = app.audio_session_coordinator_.Begin(AudioSessionMode::kLocalRecording);
        assert(recording);
        app.PollWatchFirmware();
        state = app.BuildWatchFirmwareSnapshot();
        assert(!state.model_validation_active && state.model_trial == asset_storage::TrialStatus::kPending &&
            app.audio_session_coordinator_.Mode() == AudioSessionMode::kTransitioning &&
            AudreamPowerManager::GetInstance().storage_leases == 0);
        assert(app.audio_session_coordinator_.Complete(recording));
    }
    {
        Reset(); Application app; SelectModels(app); ConfirmBoot(app);
        auto& assets = Assets::GetInstance();
        assets.trial_state = asset_storage::TrialStatus::kPending;
        WatchViewRequest validate;
        validate.id = 2; validate.action = WatchViewAction::kValidateModels;
        validate.update_target = watch_update::Target::kModels;
        assert(app.HandleWatchFirmwareRequest(validate));
        app.audio_service_.model_validation_generation = 0;
        app.PollWatchFirmware();
        auto state = app.BuildWatchFirmwareSnapshot();
        assert(state.model_trial == asset_storage::TrialStatus::kFailed && state.can_rollback_models &&
            state.model_validation_phase == WatchModelValidationPhase::kFailed);
        WatchViewRequest rollback;
        rollback.id = 3; rollback.action = WatchViewAction::kRollbackModels;
        rollback.update_target = watch_update::Target::kModels;
        assert(!app.HandleWatchFirmwareRequest(rollback) && app.reboots == 1);
    }
    Reset();
    std::cout << "PASS: offline model validation, cancellation, recording preemption and explicit rollback\n";
    std::cout << "PASS: model update Application target, exclusive session, cancellation, recording handoff and reboot\n";
    std::cout << "PASS: firmware Application admission, on-demand station, intent fencing, handoff, failures and reboot\n";
}
