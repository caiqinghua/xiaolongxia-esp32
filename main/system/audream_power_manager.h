#pragma once

#include <cstdint>
#include <functional>
#include <mutex>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum class AudreamPowerState {
    kActive,
    kIdle,
    kDeepSleepPreparing,
};

class AudreamPowerManager {
public:
    static AudreamPowerManager& GetInstance();

    void Start(gpio_num_t wake_gpio);
    void NotifyUserActivity(const char* reason, uint32_t active_timeout_seconds = 45);
    void NotifyMaintenanceActivity(const char* reason);
    void SetRecordingActive(bool active);
    void SetBleFileTransferActive(bool active);
    void SetTemporaryAPTransferActive(bool active);
    void SetStorageBusy(bool busy);
    void SetUsbPowered(bool powered);
    void SetBeforeDeepSleepCallback(std::function<void()> callback);
    AudreamPowerState State() const;

private:
    AudreamPowerManager() = default;

    mutable std::mutex mutex_;
    TaskHandle_t task_handle_ = nullptr;
    gpio_num_t wake_gpio_ = GPIO_NUM_NC;
    AudreamPowerState state_ = AudreamPowerState::kActive;
    int64_t last_user_activity_us_ = 0;
    int64_t active_timeout_us_ = 45LL * 1000000LL;
    int64_t idle_started_us_ = 0;
    bool recording_active_ = false;
    bool ble_file_transfer_active_ = false;
    bool temporary_ap_transfer_active_ = false;
    bool storage_busy_ = false;
    bool usb_powered_ = false;
    std::function<void()> before_deep_sleep_callback_;

    static void TaskEntry(void* arg);
    void Run();
    void Tick();
    bool HasBlockerLocked() const;
    void EnterActiveLocked(const char* reason, int64_t now_us);
    void EnterIdleLocked(int64_t now_us);
    void EnterDeepSleep();
};
