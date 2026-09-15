#pragma once

#include "watch_firmware_update.h"
#include "asset_trial_status.h"

enum class WatchUpdateBlock { kNone, kWifiDisabled, kNoCredentials, kBusy, kBattery, kNotConfigured };
enum class WatchModelValidationPhase { kIdle, kRunning, kSucceeded, kFailed, kCleanupFailed, kCancelled };

struct WatchFirmwareSnapshot {
    bool available = false;
    bool can_check = false;
    bool can_install = false;
    bool can_reboot = false;
    bool can_validate_models = false;
    bool can_rollback_models = false;
    bool model_validation_active = false;
    bool network_preparing = false;
    uint64_t model_validation_request = 0;
    unsigned model_validation_progress = 0;
    WatchModelValidationPhase model_validation_phase = WatchModelValidationPhase::kIdle;
    asset_storage::TrialStatus model_trial = asset_storage::TrialStatus::kNone;
    uint64_t request = 0;
    std::string current_version;
    bool current_builtin = false;
    WatchUpdateBlock blocked = WatchUpdateBlock::kNotConfigured;
    watch_update::Snapshot update;
};
