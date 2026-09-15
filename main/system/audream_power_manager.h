#pragma once

#include <cstdint>
#include <functional>
#include <mutex>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class AudioSessionCoordinator;

enum class AudreamPowerState {
    kActive,
    kIdle,
    kDeepSleepPreparing,
};

class AudreamPowerManager {
public:
    static AudreamPowerManager& GetInstance();

    // Startup defaults remain independent of screen idle and microphone permission.
    void Start(gpio_num_t wake_gpio, bool idle_deep_sleep_enabled = true);
    bool SetIdleDeepSleepEnabled(bool enabled);
    bool IdleDeepSleepEnabled() const;
    void NotifyUserActivity(const char* reason, uint32_t active_timeout_seconds = 45);
    void NotifyMaintenanceActivity(const char* reason);
    void SetRecordingActive(bool active);
    void SetRecordingCaptureActive(bool active);
    void SetBleFileTransferActive(bool active);
    void SetTemporaryAPTransferActive(bool active);
    void SetStorageBusy(bool busy);
    bool BeginStorageRead();
    void EndStorageRead();
    bool BeginVoiceNetworkRequest(bool allow_idle = false);
    void EndVoiceNetworkRequest();
    void SetUsbPowered(bool powered);
    void SetBeforeDeepSleepCallback(std::function<void()> callback);
    void SetAudioSessionCoordinator(AudioSessionCoordinator* coordinator,
        std::function<bool()> quiesce_callback, std::function<void()> resume_callback = {});
    void SetPeripheralSleepCallbacks(std::function<bool()> prepare, std::function<bool()> restore);
    void SetSleepPreparationCallbacks(std::function<bool()> prepare,
        std::function<bool(uint64_t&)> wake_delay, std::function<void()> cancel);
    AudreamPowerState State() const;
    bool IsDisplayIdle() const;
    bool TryWakeDisplayForMotion();

private:
    friend class AudreamPowerManagerTest;
    AudreamPowerManager() = default;

    mutable std::mutex mutex_;
    TaskHandle_t task_handle_ = nullptr;
    gpio_num_t wake_gpio_ = GPIO_NUM_NC;
    AudreamPowerState state_ = AudreamPowerState::kActive;
    int64_t last_user_activity_us_ = 0;
    int64_t active_timeout_us_ = 45LL * 1000000LL;
    int64_t idle_started_us_ = 0;
    int64_t display_activity_us_ = -1;
    int64_t display_timeout_us_ = 45LL * 1000000LL;
    bool idle_deep_sleep_enabled_ = true;
    bool recording_active_ = false;
    bool recording_capture_active_ = false;
    bool ble_file_transfer_active_ = false;
    bool temporary_ap_transfer_active_ = false;
    bool storage_busy_ = false;
    uint32_t storage_readers_ = 0;
    uint32_t voice_network_requests_ = 0;
    bool usb_powered_ = false;
    int current_cpu_max_mhz_ = 0;
    int current_cpu_min_mhz_ = 0;
    std::function<void()> before_deep_sleep_callback_;
    AudioSessionCoordinator* audio_coordinator_ = nullptr;
    std::function<bool()> quiesce_audio_callback_;
    std::function<void()> resume_audio_callback_;
    std::function<bool()> prepare_peripherals_callback_;
    std::function<bool()> restore_peripherals_callback_;
    std::function<bool()> pending_sleep_recovery_;
    bool sleep_recovery_in_progress_ = false;
    int64_t next_sleep_recovery_us_ = 0;
    std::function<bool()> prepare_sleep_callback_;
    std::function<bool(uint64_t&)> sleep_wake_delay_callback_;
    std::function<void()> cancel_sleep_callback_;

    static void TaskEntry(void* arg);
    void Run();
    void Tick();
    bool RetrySleepRecovery();
    bool HasBlockerLocked(bool include_audio = true) const;
    bool IsDisplayIdleLocked(int64_t now_us) const;
    void EnterActiveLocked(const char* reason, int64_t now_us);
    void EnterIdleLocked(int64_t now_us);
    void EnterDeepSleep();
    void UpdateCpuProfileLocked(const char* reason);
};
