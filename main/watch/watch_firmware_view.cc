#include "watch_view.h"
#include "assets/lang_config.h"
#include <font_awesome.h>
#include <algorithm>
#include <cstdio>

namespace {
const char* Status(watch_update::Phase phase) {
    using namespace Lang::Strings;
    using watch_update::Phase;
    switch (phase) {
        case Phase::kIdle: return WATCH_UPDATE_IDLE;
        case Phase::kChecking: return WATCH_UPDATE_CHECKING;
        case Phase::kAvailable: return WATCH_UPDATE_AVAILABLE;
        case Phase::kNoUpdate: return WATCH_UPDATE_CURRENT;
        case Phase::kNotConfigured: return WATCH_UPDATE_UNAVAILABLE;
        case Phase::kCancelled: return WATCH_UPDATE_CANCELLED;
        case Phase::kExpired: return WATCH_UPDATE_EXPIRED;
        case Phase::kInstalling: return WATCH_UPDATE_DOWNLOADING;
        case Phase::kCommitting: return WATCH_UPDATE_COMMITTING;
        case Phase::kRebootRequired: return WATCH_UPDATE_REBOOT_READY;
        case Phase::kCommitUncertain: return WATCH_UPDATE_UNCERTAIN;
        case Phase::kResourceFault: return WATCH_UPDATE_RESOURCE_FAULT;
        case Phase::kInstallFailed: return WATCH_UPDATE_INSTALL_FAILED;
        default: return WATCH_UPDATE_CHECK_FAILED;
    }
}
void Visibility(lv_obj_t* object, bool visible) {
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}
void Enabled(lv_obj_t* object, bool enabled) {
    if (enabled) lv_obj_remove_state(object, LV_STATE_DISABLED);
    else lv_obj_add_state(object, LV_STATE_DISABLED);
}
}

void WatchView::BuildFirmwareUpdate() {
    using namespace Lang::Strings;
    const bool confirmation = page_ == WatchPage::kFirmwareConfirm;
    if (!confirmation) {
        Label(body_, WATCH_UPDATE_TARGET);
        firmware_target_ = lv_dropdown_create(body_);
        lv_obj_set_width(firmware_target_, LV_PCT(100));
        lv_obj_set_height(firmware_target_, 52);
        lv_obj_set_ext_click_area(firmware_target_, 6);
        lv_dropdown_set_symbol(firmware_target_, FONT_AWESOME_ANGLE_DOWN);
        lv_obj_set_style_text_font(firmware_target_, icon_font_, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(firmware_target_, lv_color_hex(0x202427), 0);
        lv_obj_set_style_text_color(firmware_target_, lv_color_hex(0xF4F7F6), 0);
        auto* list = lv_dropdown_get_list(firmware_target_);
        lv_obj_set_style_text_font(list, text_font_, 0);
        lv_obj_set_style_bg_color(list, lv_color_hex(0x202427), 0);
        lv_obj_set_style_text_color(list, lv_color_hex(0xF4F7F6), 0);
        lv_obj_set_style_max_height(list, 200, 0);
        const std::string options = std::string(WATCH_UPDATE_FIRMWARE) + "\n" + WATCH_UPDATE_MODELS;
        lv_dropdown_set_options(firmware_target_, options.c_str());
        Bind(firmware_target_, LV_EVENT_VALUE_CHANGED, [this](lv_event_t*) {
            const auto target = lv_dropdown_get_selected(firmware_target_) == 1
                ? watch_update::Target::kModels : watch_update::Target::kFirmware;
            const auto& update = state_.firmware.update;
            if (!lv_obj_has_state(firmware_target_, LV_STATE_DISABLED) && !pending_request_ &&
                !update.active && !update.commit_started && !update.boot_selected &&
                !state_.firmware.network_preparing && update.phase != watch_update::Phase::kResourceFault &&
                target != update.target) {
                WatchViewRequest request;
                request.action = WatchViewAction::kSelectUpdateTarget;
                request.update_target = target;
                Submit(request);
            }
            RefreshFirmwareUpdate();
        });
    } else {
        Label(body_, firmware_confirmation_.target == watch_update::Target::kModels
            ? WATCH_UPDATE_MODELS : WATCH_UPDATE_FIRMWARE);
    }
    Label(body_, WATCH_UPDATE_INSTALLED_VERSION);
    firmware_installed_ = Label(body_, "");
    firmware_version_ = Label(body_, "");
    firmware_status_ = Label(body_, "");
    lv_obj_set_style_min_height(firmware_status_, 90, 0);
    firmware_progress_ = lv_bar_create(body_);
    lv_obj_set_size(firmware_progress_, LV_PCT(100), 12);
    lv_bar_set_range(firmware_progress_, 0, 100);
    lv_obj_set_style_bg_color(firmware_progress_, lv_color_hex(0x202427), LV_PART_MAIN);
    lv_obj_set_style_bg_color(firmware_progress_, lv_color_hex(0x70DDC0), LV_PART_INDICATOR);
    firmware_progress_text_ = Label(body_, "");
    firmware_check_ = Button(body_, WATCH_UPDATE_CHECK, [this] {
        if (page_ != WatchPage::kFirmwareUpdate || !state_.firmware.available || !state_.firmware.can_check ||
            state_.firmware.network_preparing || state_.firmware.update.active || state_.firmware.update.commit_started ||
            state_.firmware.update.boot_selected || state_.firmware.update.phase == watch_update::Phase::kAvailable) return;
        WatchViewRequest request;
        request.action = WatchViewAction::kCheckFirmware;
        request.update_target = state_.firmware.update.target;
        Submit(request);
    });
    firmware_install_ = Button(body_, confirmation ? WATCH_UPDATE_INSTALL : WATCH_UPDATE_REVIEW, [this] {
        if (!state_.firmware.can_install || state_.firmware.update.phase != watch_update::Phase::kAvailable) return;
        if (page_ != WatchPage::kFirmwareConfirm) {
            if (!pending_request_) Navigate(WatchPage::kFirmwareConfirm);
            return;
        }
        const auto& update = state_.firmware.update;
        if (update.attempt != firmware_confirmation_.attempt || update.sequence != firmware_confirmation_.sequence ||
            update.version != firmware_confirmation_.version || update.target != firmware_confirmation_.target) return;
        WatchViewRequest request;
        request.action = WatchViewAction::kInstallFirmware;
        request.update_target = firmware_confirmation_.target;
        request.firmware_request = state_.firmware.request;
        request.firmware_attempt = firmware_confirmation_.attempt;
        request.firmware_sequence = firmware_confirmation_.sequence;
        Submit(request);
    });
    firmware_cancel_ = Button(body_, WATCH_UPDATE_CANCEL, [this] { CancelFirmware(); });
    firmware_reboot_ = Button(body_, WATCH_UPDATE_RESTART, [this] {
        if (!state_.firmware.can_reboot || !state_.firmware.update.boot_selected) return;
        WatchViewRequest request;
        request.action = WatchViewAction::kRebootFirmware;
        request.update_target = state_.firmware.update.target;
        request.firmware_request = state_.firmware.request;
        request.firmware_attempt = state_.firmware.update.attempt;
        request.firmware_sequence = state_.firmware.update.sequence;
        Submit(request);
    });
    firmware_validate_ = Button(body_, WATCH_MODELS_VALIDATE, [this] {
        if (!state_.firmware.can_validate_models || state_.firmware.update.target != watch_update::Target::kModels)
            return;
        WatchViewRequest request;
        request.action = WatchViewAction::kValidateModels;
        request.update_target = watch_update::Target::kModels;
        Submit(request);
    });
    firmware_rollback_ = Button(body_, WATCH_MODELS_ROLLBACK, [this] {
        if (!state_.firmware.can_rollback_models || state_.firmware.update.target != watch_update::Target::kModels)
            return;
        WatchViewRequest request;
        request.action = WatchViewAction::kRollbackModels;
        request.update_target = watch_update::Target::kModels;
        Submit(request);
    });
    RefreshFirmwareUpdate();
}

void WatchView::RefreshFirmwareUpdate() {
    using namespace Lang::Strings;
    using watch_update::Phase;
    const auto& firmware = state_.firmware;
    const auto& update = firmware.update;
    const bool confirmation = page_ == WatchPage::kFirmwareConfirm;
    const bool same = update.attempt == firmware_confirmation_.attempt &&
        update.sequence == firmware_confirmation_.sequence && update.version == firmware_confirmation_.version &&
        update.target == firmware_confirmation_.target;
    const bool available = update.phase == Phase::kAvailable;
    const bool pending = pending_request_ && (pending_action_ == WatchViewAction::kCheckFirmware ||
        pending_action_ == WatchViewAction::kInstallFirmware);
    const bool committed = update.commit_started || update.boot_selected;
    const bool models = update.target == watch_update::Target::kModels;
    const bool model_validation_message = models && !confirmation &&
        (firmware.model_trial != asset_storage::TrialStatus::kNone ||
         firmware.model_validation_phase != WatchModelValidationPhase::kIdle);
    const bool cancel = !committed && update.phase != Phase::kResourceFault &&
        (pending || firmware.network_preparing || update.active || available);
    const bool usable = handler_ && visible_ && !touch_locked_ && !state_.time.sleep_prepared && firmware.available;
    if (firmware_target_) {
        lv_dropdown_set_selected(firmware_target_, update.target == watch_update::Target::kModels ? 1 : 0);
        Enabled(firmware_target_, usable && !pending_request_ && !update.active && !committed &&
            !firmware.network_preparing && update.phase != Phase::kResourceFault &&
            firmware.blocked != WatchUpdateBlock::kBusy);
    }
    lv_label_set_text(firmware_installed_, confirmation && !same ? "--" :
        firmware.current_builtin ? WATCH_UPDATE_BUILT_IN :
        firmware.current_version.empty() ? "--" : firmware.current_version.c_str());
    const char* status = Status(update.phase);
    if (firmware.network_preparing) status = WATCH_UPDATE_CONNECTING;
    else if (confirmation && available) status = same ? WATCH_UPDATE_CONFIRM : WATCH_UPDATE_EXPIRED;
    if (update.cancelling && update.active) status = WATCH_UPDATE_STOPPING;
    if (models && !confirmation) {
        if (firmware.model_validation_active || firmware.model_trial == asset_storage::TrialStatus::kObserving)
            status = WATCH_MODELS_TRIAL_RUNNING;
        else if (firmware.model_validation_phase == WatchModelValidationPhase::kSucceeded)
            status = WATCH_MODELS_TRIAL_PASSED;
        else if (firmware.model_validation_phase == WatchModelValidationPhase::kCancelled)
            status = WATCH_MODELS_TRIAL_CANCELLED;
        else if (firmware.model_trial == asset_storage::TrialStatus::kFailed ||
                 firmware.model_validation_phase == WatchModelValidationPhase::kFailed ||
                 firmware.model_validation_phase == WatchModelValidationPhase::kCleanupFailed)
            status = WATCH_MODELS_TRIAL_FAILED;
        else if (firmware.model_trial == asset_storage::TrialStatus::kPending)
            status = WATCH_MODELS_TRIAL_PENDING;
    }
    if (!firmware.available) status = WATCH_UPDATE_UNAVAILABLE;
    else if (!model_validation_message && !update.active && !committed && !firmware.network_preparing &&
             (update.phase == Phase::kIdle || available)) {
        switch (firmware.blocked) {
            case WatchUpdateBlock::kWifiDisabled: status = WATCH_UPDATE_WIFI_OFF; break;
            case WatchUpdateBlock::kNoCredentials: status = WATCH_UPDATE_NO_WIFI; break;
            case WatchUpdateBlock::kBusy: status = WATCH_UPDATE_BUSY; break;
            case WatchUpdateBlock::kBattery: status = WATCH_UPDATE_BATTERY; break;
            case WatchUpdateBlock::kNotConfigured: status = WATCH_UPDATE_UNAVAILABLE; break;
            default: break;
        }
    }
    lv_label_set_text(firmware_status_, status);
    const auto& selected = confirmation ? firmware_confirmation_ : update;
    char version[128];
    std::snprintf(version, sizeof(version), "%s\n%s / %u KiB", WATCH_UPDATE_NEW_VERSION,
                  selected.version.c_str(), static_cast<unsigned>((selected.bytes + 1023) / 1024));
    lv_label_set_text(firmware_version_, version);
    Visibility(firmware_version_, !selected.version.empty());
    const bool progress = update.phase == Phase::kInstalling || committed || firmware.model_validation_active;
    Visibility(firmware_progress_, progress);
    Visibility(firmware_progress_text_, progress);
    const auto percent = firmware.model_validation_active ? std::min(firmware.model_validation_progress, 99U)
        : std::min(update.progress, update.boot_selected ? 100U : 99U);
    lv_bar_set_value(firmware_progress_, percent, LV_ANIM_OFF);
    char text[48];
    if (firmware.model_validation_active) std::snprintf(text, sizeof(text), "%u%%", percent);
    else std::snprintf(text, sizeof(text), "%u%%  %u KiB/s", percent, static_cast<unsigned>(update.bytes_per_second / 1024));
    lv_label_set_text(firmware_progress_text_, text);
    Visibility(firmware_check_, !model_validation_message && !available && !update.active && !committed &&
        !firmware.network_preparing && !confirmation);
    Enabled(firmware_check_, usable && !pending_request_ && firmware.can_check);
    Visibility(firmware_install_, available);
    Enabled(firmware_install_, usable && !pending_request_ && firmware.can_install && (!confirmation || same));
    Visibility(firmware_cancel_, cancel || firmware.model_validation_active);
    if (firmware_cancel_ && lv_obj_get_child_count(firmware_cancel_))
        lv_label_set_text(lv_obj_get_child(firmware_cancel_, 0),
            firmware.model_validation_active ? WATCH_MODELS_CANCEL : WATCH_UPDATE_CANCEL);
    Enabled(firmware_cancel_, usable && !update.cancelling);
    Visibility(firmware_reboot_, update.boot_selected);
    Enabled(firmware_reboot_, usable && !pending_request_ && firmware.can_reboot);
    Visibility(firmware_validate_, !confirmation && models && firmware.model_trial == asset_storage::TrialStatus::kPending &&
        !firmware.model_validation_active);
    Enabled(firmware_validate_, usable && !pending_request_ && firmware.can_validate_models);
    Visibility(firmware_rollback_, !confirmation && models && firmware.can_rollback_models);
    Enabled(firmware_rollback_, usable && !pending_request_ && firmware.can_rollback_models);
}

void WatchView::CancelFirmware() {
    if (state_.firmware.model_validation_active) {
        WatchViewRequest request;
        request.action = WatchViewAction::kCancelModelValidation;
        request.update_target = watch_update::Target::kModels;
        request.firmware_request = state_.firmware.model_validation_request;
        Submit(request);
        return;
    }
    const auto& update = state_.firmware.update;
    const bool pending = pending_request_ && (pending_action_ == WatchViewAction::kCheckFirmware ||
        pending_action_ == WatchViewAction::kInstallFirmware);
    if (update.commit_started || update.boot_selected || update.phase == watch_update::Phase::kResourceFault ||
        (!pending && !update.active && !state_.firmware.network_preparing && update.phase != watch_update::Phase::kAvailable)) return;
    WatchViewRequest request;
    request.action = WatchViewAction::kCancelFirmware;
    request.update_target = update.target;
    request.firmware_request = pending_action_ == WatchViewAction::kCheckFirmware && pending_request_
        ? firmware_check_request_ : state_.firmware.request ? state_.firmware.request : firmware_check_request_;
    request.firmware_attempt = update.attempt;
    Submit(request);
}
