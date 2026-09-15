#include "system/audream_power_manager.h"
#include "audio/audio_session_coordinator.h"

#include <utility>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "soc/soc_caps.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "AudreamPower";
constexpr int64_t kIdleToDeepSleepUs = 30LL * 1000000LL;
constexpr int64_t kTickDelayMs = 1000;
#if CONFIG_IDF_TARGET_ESP32C6
constexpr int kDefaultCpuMaxMhz = 160;
#else
constexpr int kDefaultCpuMaxMhz = 240;
#endif
constexpr int kDefaultCpuMinMhz = 40;
constexpr int kRecordingCpuMaxMhz = 80;
constexpr int kRecordingCpuMinMhz = 40;

const char* StateName(AudreamPowerState state) {
    switch (state) {
        case AudreamPowerState::kActive:
            return "active";
        case AudreamPowerState::kIdle:
            return "idle";
        case AudreamPowerState::kDeepSleepPreparing:
            return "deep_sleep_preparing";
    }
    return "unknown";
}

}  // namespace

AudreamPowerManager& AudreamPowerManager::GetInstance() {
    static AudreamPowerManager instance;
    return instance;
}

void AudreamPowerManager::Start(gpio_num_t wake_gpio, bool idle_deep_sleep_enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_handle_ != nullptr) {
        return;
    }
    wake_gpio_ = wake_gpio;
    idle_deep_sleep_enabled_ = idle_deep_sleep_enabled;
    const int64_t now_us = esp_timer_get_time();
    if (last_user_activity_us_ == 0) {
        last_user_activity_us_ = now_us;
    }
    if (display_activity_us_ < 0) display_activity_us_ = now_us;
    const BaseType_t created = xTaskCreate(
        &AudreamPowerManager::TaskEntry,
        "audream_power",
        4096,
        this,
        3,
        &task_handle_
    );
    if (created != pdPASS) {
        task_handle_ = nullptr;
        ESP_LOGE(kTag, "Failed to create power manager task");
        return;
    }
    ESP_LOGI(kTag, "Power manager started, wake_gpio=%d idle_deep_sleep=%d",
        static_cast<int>(wake_gpio_), idle_deep_sleep_enabled_ ? 1 : 0);
}

bool AudreamPowerManager::SetIdleDeepSleepEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!task_handle_ || state_ == AudreamPowerState::kDeepSleepPreparing || pending_sleep_recovery_) return false;
    if (idle_deep_sleep_enabled_ == enabled) return true;
    idle_deep_sleep_enabled_ = enabled;
    // A newly selected saver gets a full grace period, without waking the screen.
    if (state_ == AudreamPowerState::kIdle) idle_started_us_ = esp_timer_get_time();
    ESP_LOGI(kTag, "Idle deep sleep policy changed: enabled=%d", enabled ? 1 : 0);
    return true;
}

bool AudreamPowerManager::IdleDeepSleepEnabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return idle_deep_sleep_enabled_;
}

void AudreamPowerManager::NotifyUserActivity(
    const char* reason,
    uint32_t active_timeout_seconds
) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t now_us = esp_timer_get_time();
    last_user_activity_us_ = now_us;
    active_timeout_us_ = static_cast<int64_t>(active_timeout_seconds) * 1000000LL;
    display_activity_us_ = now_us;
    display_timeout_us_ = active_timeout_us_;
    if (state_ != AudreamPowerState::kActive) {
        EnterActiveLocked(reason, now_us);
        return;
    }
    ESP_LOGI(
        kTag,
        "User activity: %s active_timeout=%lu",
        reason == nullptr ? "unknown" : reason,
        static_cast<unsigned long>(active_timeout_seconds)
    );
}

void AudreamPowerManager::NotifyMaintenanceActivity(const char* reason) {
    ESP_LOGD(kTag, "Maintenance activity: %s", reason == nullptr ? "unknown" : reason);
}

void AudreamPowerManager::SetRecordingActive(bool active) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recording_active_ == active) {
        return;
    }
    recording_active_ = active;
    recording_capture_active_ = active;
    UpdateCpuProfileLocked(active ? "recording_started" : "recording_stopped");
    if (active) {
        const int64_t now_us = esp_timer_get_time();
        last_user_activity_us_ = now_us;
        active_timeout_us_ = 45LL * 1000000LL;
        EnterActiveLocked("recording_started", now_us);
        display_activity_us_ = now_us;
        display_timeout_us_ = active_timeout_us_;
    }
    ESP_LOGI(kTag, "Recording blocker: %s", active ? "active" : "inactive");
}

void AudreamPowerManager::SetRecordingCaptureActive(bool active) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recording_capture_active_ == active) {
        return;
    }
    recording_capture_active_ = active;
    UpdateCpuProfileLocked(active ? "recording_capture_started" : "recording_capture_stopped");
}

void AudreamPowerManager::UpdateCpuProfileLocked(const char* reason) {
#if CONFIG_PM_ENABLE
    const bool use_recording_profile = recording_capture_active_ &&
        !ble_file_transfer_active_ &&
        !temporary_ap_transfer_active_ &&
        !storage_busy_ && storage_readers_ == 0 && voice_network_requests_ == 0;
    const int target_max_mhz = use_recording_profile ? kRecordingCpuMaxMhz : kDefaultCpuMaxMhz;
    const int target_min_mhz = use_recording_profile ? kRecordingCpuMinMhz : kDefaultCpuMinMhz;
    if (current_cpu_max_mhz_ == target_max_mhz && current_cpu_min_mhz_ == target_min_mhz) {
        return;
    }
    const esp_pm_config_t config = {
        .max_freq_mhz = target_max_mhz,
        .min_freq_mhz = target_min_mhz,
        .light_sleep_enable = true,
    };
    const esp_err_t result = esp_pm_configure(&config);
    if (result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "Failed to apply CPU power profile after %s: %s",
            reason == nullptr ? "state_change" : reason,
            esp_err_to_name(result)
        );
        return;
    }
    current_cpu_max_mhz_ = target_max_mhz;
    current_cpu_min_mhz_ = target_min_mhz;
    ESP_LOGI(
        kTag,
        "CPU power profile: %s max=%dMHz min=%dMHz reason=%s",
        use_recording_profile ? "recording" : "default",
        static_cast<int>(config.max_freq_mhz),
        static_cast<int>(config.min_freq_mhz),
        reason == nullptr ? "state_change" : reason
    );
#else
    (void)reason;
#endif
}

void AudreamPowerManager::SetBleFileTransferActive(bool active) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ble_file_transfer_active_ == active) {
        return;
    }
    ble_file_transfer_active_ = active;
    UpdateCpuProfileLocked(active ? "ble_file_transfer_started" : "ble_file_transfer_stopped");
    if (active) {
        const int64_t now_us = esp_timer_get_time();
        last_user_activity_us_ = now_us;
        active_timeout_us_ = 45LL * 1000000LL;
        EnterActiveLocked("ble_file_transfer_started", now_us);
    }
    ESP_LOGD(kTag, "BLE file transfer blocker: %s", active ? "active" : "inactive");
}

void AudreamPowerManager::SetTemporaryAPTransferActive(bool active) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (temporary_ap_transfer_active_ == active) {
        return;
    }
    temporary_ap_transfer_active_ = active;
    UpdateCpuProfileLocked(active ? "temporary_ap_transfer_started" : "temporary_ap_transfer_stopped");
    if (active) {
        const int64_t now_us = esp_timer_get_time();
        last_user_activity_us_ = now_us;
        active_timeout_us_ = 45LL * 1000000LL;
        EnterActiveLocked("temporary_ap_transfer_started", now_us);
    }
    ESP_LOGI(kTag, "Temporary AP transfer blocker: %s", active ? "active" : "inactive");
}

bool AudreamPowerManager::BeginStorageRead() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == AudreamPowerState::kDeepSleepPreparing || pending_sleep_recovery_ || storage_readers_ == UINT32_MAX) return false;
    ++storage_readers_;
    UpdateCpuProfileLocked("storage_read_started");
    return true;
}

void AudreamPowerManager::EndStorageRead() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (storage_readers_ == 0) {
        ESP_LOGE(kTag, "Unbalanced storage read release");
        return;
    }
    --storage_readers_;
    UpdateCpuProfileLocked("storage_read_stopped");
}

void AudreamPowerManager::SetStorageBusy(bool busy) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (storage_busy_ == busy) {
        return;
    }
    storage_busy_ = busy;
    UpdateCpuProfileLocked(busy ? "storage_started" : "storage_stopped");
    ESP_LOGD(kTag, "Storage blocker: %s", busy ? "active" : "inactive");
}

bool AudreamPowerManager::BeginVoiceNetworkRequest(bool allow_idle) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool idle_allowed = allow_idle && !idle_deep_sleep_enabled_ && state_ == AudreamPowerState::kIdle;
    if ((state_ != AudreamPowerState::kActive && !idle_allowed) || pending_sleep_recovery_ ||
        voice_network_requests_ == UINT32_MAX) return false;
    ++voice_network_requests_;
    UpdateCpuProfileLocked("voice_network_started");
    return true;
}

void AudreamPowerManager::EndVoiceNetworkRequest() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (voice_network_requests_ == 0) {
        ESP_LOGE(kTag, "Unbalanced voice network release");
        return;
    }
    --voice_network_requests_;
    UpdateCpuProfileLocked("voice_network_stopped");
}

void AudreamPowerManager::SetUsbPowered(bool powered) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (usb_powered_ == powered) {
        return;
    }
    usb_powered_ = powered;
    ESP_LOGI(kTag, "USB power state: %s", powered ? "powered" : "not_powered");
}

void AudreamPowerManager::SetAudioSessionCoordinator(AudioSessionCoordinator* coordinator,
    std::function<bool()> quiesce_callback, std::function<void()> resume_callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    audio_coordinator_ = coordinator;
    quiesce_audio_callback_ = std::move(quiesce_callback);
    resume_audio_callback_ = std::move(resume_callback);
}

void AudreamPowerManager::SetPeripheralSleepCallbacks(std::function<bool()> prepare,
    std::function<bool()> restore) {
    std::lock_guard<std::mutex> lock(mutex_);
    prepare_peripherals_callback_ = std::move(prepare);
    restore_peripherals_callback_ = std::move(restore);
}

void AudreamPowerManager::SetBeforeDeepSleepCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    before_deep_sleep_callback_ = std::move(callback);
}

void AudreamPowerManager::SetSleepPreparationCallbacks(std::function<bool()> prepare,
    std::function<bool(uint64_t&)> wake_delay, std::function<void()> cancel) {
    std::lock_guard<std::mutex> lock(mutex_);
    prepare_sleep_callback_ = std::move(prepare);
    sleep_wake_delay_callback_ = std::move(wake_delay);
    cancel_sleep_callback_ = std::move(cancel);
}

AudreamPowerState AudreamPowerManager::State() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void AudreamPowerManager::TaskEntry(void* arg) {
    auto* manager = static_cast<AudreamPowerManager*>(arg);
    if (manager != nullptr) {
        manager->Run();
    }
    vTaskDelete(nullptr);
}

void AudreamPowerManager::Run() {
    while (true) {
        Tick();
        vTaskDelay(pdMS_TO_TICKS(kTickDelayMs));
    }
}

void AudreamPowerManager::Tick() {
    if (RetrySleepRecovery()) { return; }
    bool should_enter_deep_sleep = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t now_us = esp_timer_get_time();
        if (last_user_activity_us_ == 0) {
            last_user_activity_us_ = now_us;
        }

        if (state_ == AudreamPowerState::kActive) {
            if (!HasBlockerLocked() && now_us - last_user_activity_us_ >= active_timeout_us_) {
                EnterIdleLocked(now_us);
            }
            return;
        }

        if (state_ == AudreamPowerState::kIdle) {
            if (HasBlockerLocked()) {
                EnterActiveLocked("blocker_started", now_us);
                return;
            }
            if (!idle_deep_sleep_enabled_ || now_us - idle_started_us_ < kIdleToDeepSleepUs) {
                return;
            }
            state_ = AudreamPowerState::kDeepSleepPreparing;
            should_enter_deep_sleep = true;
        }
    }

    if (should_enter_deep_sleep) {
        EnterDeepSleep();
    }
}

bool AudreamPowerManager::HasBlockerLocked(bool include_audio) const {
    return pending_sleep_recovery_ || recording_active_ ||
        ble_file_transfer_active_ ||
        temporary_ap_transfer_active_ ||
        storage_busy_ || storage_readers_ > 0 || voice_network_requests_ > 0 ||
        (include_audio && audio_coordinator_ && audio_coordinator_->BlocksSleep());
}

bool AudreamPowerManager::RetrySleepRecovery() {
    std::function<bool()> recover;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_sleep_recovery_) { return false; }
        if (sleep_recovery_in_progress_ || esp_timer_get_time() < next_sleep_recovery_us_) { return true; }
        sleep_recovery_in_progress_ = true;
        recover = pending_sleep_recovery_;
    }
    const bool recovered = recover();
    std::lock_guard<std::mutex> lock(mutex_);
    sleep_recovery_in_progress_ = false;
    if (recovered) {
        pending_sleep_recovery_ = {};
        EnterActiveLocked("sleep_recovery_completed", esp_timer_get_time());
    } else {
        next_sleep_recovery_us_ = esp_timer_get_time() + 5000000;
        ESP_LOGE(kTag, "Resource recovery failed; keeping sleep reservation for retry");
    }
    return true;
}

void AudreamPowerManager::EnterActiveLocked(const char* reason, int64_t now_us) {
    const auto previous_state = state_;
    state_ = AudreamPowerState::kActive;
    idle_started_us_ = 0;
    last_user_activity_us_ = now_us;
    ESP_LOGI(
        kTag,
        "Power state %s -> active reason=%s",
        StateName(previous_state),
        reason == nullptr ? "unknown" : reason
    );
}

void AudreamPowerManager::EnterIdleLocked(int64_t now_us) {
    state_ = AudreamPowerState::kIdle;
    idle_started_us_ = now_us;
    ESP_LOGI(kTag, "Power state active -> idle");
}

bool AudreamPowerManager::IsDisplayIdle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return IsDisplayIdleLocked(esp_timer_get_time());
}

bool AudreamPowerManager::IsDisplayIdleLocked(int64_t now_us) const {
    // Maintenance may keep the CPU active without being a display interaction.
    // Keep an active assistant visible so hiding the view cannot cancel its PTT.
    if (audio_coordinator_ && AudioSessionCoordinator::IsAssistant(audio_coordinator_->Mode())) return false;
    return display_activity_us_ >= 0 && now_us >= display_activity_us_ &&
        now_us - display_activity_us_ >= display_timeout_us_;
}

bool AudreamPowerManager::TryWakeDisplayForMotion() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now_us = esp_timer_get_time();
    if (state_ == AudreamPowerState::kDeepSleepPreparing || HasBlockerLocked() || !IsDisplayIdleLocked(now_us)) return false;
    active_timeout_us_ = 10LL * 1000000LL;
    display_activity_us_ = now_us;
    display_timeout_us_ = active_timeout_us_;
    EnterActiveLocked("watch_raise_to_wake", now_us);
    return true;
}

void AudreamPowerManager::EnterDeepSleep() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!idle_deep_sleep_enabled_) {
            if (state_ == AudreamPowerState::kDeepSleepPreparing) {
                state_ = AudreamPowerState::kIdle;
                idle_started_us_ = esp_timer_get_time();
            }
            return;
        }
    }
    ESP_LOGI(kTag, "Entering deep sleep");
    const esp_err_t disable_result = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (disable_result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "Failed to clear existing wake sources before deep sleep: %s",
            esp_err_to_name(disable_result)
        );
        std::lock_guard<std::mutex> lock(mutex_);
        EnterActiveLocked("wake_source_reset_failed", esp_timer_get_time());
        return;
    }
    if (wake_gpio_ != GPIO_NUM_NC) {
        gpio_set_direction(wake_gpio_, GPIO_MODE_INPUT);
        gpio_pullup_en(wake_gpio_);
        gpio_pulldown_dis(wake_gpio_);
        vTaskDelay(pdMS_TO_TICKS(20));
        const int wake_level = gpio_get_level(wake_gpio_);
        ESP_LOGI(
            kTag,
            "Deep sleep wake GPIO %d level before sleep: %d",
            static_cast<int>(wake_gpio_),
            wake_level
        );
        if (wake_level == 0) {
            ESP_LOGW(
                kTag,
                "Deep sleep canceled because wake GPIO %d is already active",
                static_cast<int>(wake_gpio_)
            );
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = AudreamPowerState::kIdle;
            idle_started_us_ = esp_timer_get_time();
            return;
        }
#if SOC_PM_SUPPORT_EXT0_WAKEUP
        const esp_err_t wake_result = esp_sleep_enable_ext0_wakeup(wake_gpio_, 0);
#elif SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
        const esp_err_t wake_result = esp_deep_sleep_enable_gpio_wakeup(
            1ULL << static_cast<uint32_t>(wake_gpio_),
            ESP_GPIO_WAKEUP_GPIO_LOW
        );
#else
        const esp_err_t wake_result = ESP_ERR_NOT_SUPPORTED;
#endif
        if (wake_result != ESP_OK) {
            ESP_LOGE(
                kTag,
                "Failed to configure deep sleep wake GPIO %d: %s",
                static_cast<int>(wake_gpio_),
                esp_err_to_name(wake_result)
            );
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = AudreamPowerState::kIdle;
            return;
        }
    }

    std::function<void()> before_sleep_callback;
    std::function<bool()> quiesce_audio;
    std::function<void()> resume_audio;
    std::function<bool()> prepare_peripherals;
    std::function<bool()> restore_peripherals;
    std::function<bool()> prepare_sleep;
    std::function<bool(uint64_t&)> wake_delay;
    std::function<void()> cancel_sleep;
    AudioSessionCoordinator* coordinator = nullptr;
    AudioSessionTransition sleep_transition;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != AudreamPowerState::kDeepSleepPreparing || HasBlockerLocked()) {
            EnterActiveLocked("sleep_preparation_interrupted", esp_timer_get_time());
            return;
        }
        coordinator = audio_coordinator_;
        quiesce_audio = quiesce_audio_callback_;
        resume_audio = resume_audio_callback_;
        prepare_peripherals = prepare_peripherals_callback_;
        restore_peripherals = restore_peripherals_callback_;
        prepare_sleep = prepare_sleep_callback_;
        wake_delay = sleep_wake_delay_callback_;
        cancel_sleep = cancel_sleep_callback_;
        if (coordinator) {
            sleep_transition = coordinator->Begin(AudioSessionMode::kSleeping);
            if (!sleep_transition) {
                EnterActiveLocked("audio_session_busy", esp_timer_get_time());
                return;
            }
        }
        before_sleep_callback = before_deep_sleep_callback_;
    }
    bool peripherals_started = false;
    bool audio_quiesced = false;
    const auto abort_sleep = [&](const char* reason, bool audio_released) {
        const auto recover = [coordinator, sleep_transition, restore_peripherals, peripherals_started,
            resume_audio, quiesce_audio, audio_quiesced, audio_released]() {
            if (!audio_released && (!quiesce_audio || !quiesce_audio())) { return false; }
            if (peripherals_started && !restore_peripherals()) { return false; }
            if (coordinator && !coordinator->Fail(sleep_transition, true)) { return false; }
            if ((audio_quiesced || !audio_released) && resume_audio) { resume_audio(); }
            return true;
        };
        // An exhausted close/drain deadline is retried later, not immediately.
        const bool recovered = audio_released && recover();
        if (cancel_sleep) { cancel_sleep(); }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!recovered) {
            pending_sleep_recovery_ = recover;
            next_sleep_recovery_us_ = esp_timer_get_time() + 5000000;
        }
        EnterActiveLocked(reason, esp_timer_get_time());
    };
    if (prepare_sleep && (!wake_delay || !cancel_sleep || !prepare_sleep())) {
        abort_sleep("sleep_checkpoint_failed", true);
        return;
    }
    if (quiesce_audio && !quiesce_audio()) {
        abort_sleep("audio_shutdown_failed", false);
        return;
    }
    audio_quiesced = static_cast<bool>(quiesce_audio);
    bool interrupted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        interrupted = state_ != AudreamPowerState::kDeepSleepPreparing || HasBlockerLocked(false);
    }
    if (interrupted) {
        abort_sleep("sleep_preparation_interrupted", true);
        return;
    }
    uint64_t delay_ms = 0;
    const auto arm_timer = [](uint64_t delay) {
        if (delay == 0) { return ESP_OK; }
        // Watch timers and calendar alarms are bounded to the next eight days.
        if (delay > 8ULL * 24 * 60 * 60 * 1000) { return ESP_ERR_INVALID_ARG; }
        return esp_sleep_enable_timer_wakeup(delay * 1000);
    };
    if (wake_delay && (!wake_delay(delay_ms) || arm_timer(delay_ms) != ESP_OK)) {
        abort_sleep("wake_timer_configuration_failed", true);
        return;
    }
    if (prepare_peripherals) {
        if (!restore_peripherals) {
            abort_sleep("peripheral_restore_callback_missing", true);
            return;
        }
        peripherals_started = true;
        if (!prepare_peripherals()) {
            abort_sleep("peripheral_preparation_failed", true);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            interrupted = state_ != AudreamPowerState::kDeepSleepPreparing || HasBlockerLocked(false);
        }
        if (interrupted || (wake_gpio_ != GPIO_NUM_NC && gpio_get_level(wake_gpio_) == 0)) {
            abort_sleep("peripheral_preparation_interrupted", true);
            return;
        }
    }
    if (before_sleep_callback) {
        before_sleep_callback();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    esp_wifi_stop();
    if (wake_delay) {
        // Recompute after peripheral shutdown; an expired deadline wakes in 1 ms.
        // Keep the already-armed timer if the final re-arm unexpectedly fails.
        if (!wake_delay(delay_ms)) { delay_ms = 1; }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ != AudreamPowerState::kDeepSleepPreparing) { delay_ms = 1; }
        }
        const auto result = arm_timer(delay_ms);
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "Failed to refresh wake timer; retaining initial deadline: %s", esp_err_to_name(result));
        }
    }
    if (coordinator && !coordinator->Complete(sleep_transition)) {
        ESP_LOGE(kTag, "Sleep reservation was invalidated during shutdown");
        if (cancel_sleep) { cancel_sleep(); }
        std::lock_guard<std::mutex> lock(mutex_);
        EnterActiveLocked("sleep_reservation_lost", esp_timer_get_time());
        return;
    }
    esp_deep_sleep_start();
}
