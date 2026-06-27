#include "system/audream_power_manager.h"

#include <utility>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "AudreamPower";
constexpr int64_t kIdleToDeepSleepUs = 180LL * 1000000LL;
constexpr int64_t kTickDelayMs = 1000;

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

void AudreamPowerManager::Start(gpio_num_t wake_gpio) {
    std::lock_guard<std::mutex> lock(mutex_);
    wake_gpio_ = wake_gpio;
    const int64_t now_us = esp_timer_get_time();
    if (last_user_activity_us_ == 0) {
        last_user_activity_us_ = now_us;
    }
    if (task_handle_ != nullptr) {
        return;
    }
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
    ESP_LOGI(kTag, "Power manager started, wake_gpio=%d", static_cast<int>(wake_gpio_));
}

void AudreamPowerManager::NotifyUserActivity(
    const char* reason,
    uint32_t active_timeout_seconds
) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t now_us = esp_timer_get_time();
    last_user_activity_us_ = now_us;
    active_timeout_us_ = static_cast<int64_t>(active_timeout_seconds) * 1000000LL;
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
    if (active) {
        const int64_t now_us = esp_timer_get_time();
        last_user_activity_us_ = now_us;
        active_timeout_us_ = 45LL * 1000000LL;
        EnterActiveLocked("recording_started", now_us);
    }
    ESP_LOGI(kTag, "Recording blocker: %s", active ? "active" : "inactive");
}

void AudreamPowerManager::SetBleFileTransferActive(bool active) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ble_file_transfer_active_ == active) {
        return;
    }
    ble_file_transfer_active_ = active;
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
    if (active) {
        const int64_t now_us = esp_timer_get_time();
        last_user_activity_us_ = now_us;
        active_timeout_us_ = 45LL * 1000000LL;
        EnterActiveLocked("temporary_ap_transfer_started", now_us);
    }
    ESP_LOGI(kTag, "Temporary AP transfer blocker: %s", active ? "active" : "inactive");
}

void AudreamPowerManager::SetStorageBusy(bool busy) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (storage_busy_ == busy) {
        return;
    }
    storage_busy_ = busy;
    ESP_LOGD(kTag, "Storage blocker: %s", busy ? "active" : "inactive");
}

void AudreamPowerManager::SetUsbPowered(bool powered) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (usb_powered_ == powered) {
        return;
    }
    usb_powered_ = powered;
    ESP_LOGI(kTag, "USB power state: %s", powered ? "powered" : "not_powered");
}

void AudreamPowerManager::SetBeforeDeepSleepCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    before_deep_sleep_callback_ = std::move(callback);
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
            if (now_us - idle_started_us_ < kIdleToDeepSleepUs) {
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

bool AudreamPowerManager::HasBlockerLocked() const {
    return recording_active_ ||
        ble_file_transfer_active_ ||
        temporary_ap_transfer_active_ ||
        storage_busy_;
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

void AudreamPowerManager::EnterDeepSleep() {
    ESP_LOGI(kTag, "Entering deep sleep");
    const esp_err_t disable_result = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (disable_result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "Failed to clear existing wake sources before deep sleep: %s",
            esp_err_to_name(disable_result)
        );
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
        const esp_err_t wake_result = esp_sleep_enable_ext0_wakeup(wake_gpio_, 0);
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        before_sleep_callback = before_deep_sleep_callback_;
    }
    if (before_sleep_callback) {
        before_sleep_callback();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    esp_wifi_stop();
    esp_deep_sleep_start();
}
