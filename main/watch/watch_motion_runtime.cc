#include "watch_motion_runtime.h"

#include "watch_calendar.h"
#include "watch_time_runtime.h"
#include "watch_motion_storage.h"
#include "audio/local_recording_service.h"
#include "storage/temporary_ap_transfer_service.h"
#include "system/audream_power_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <inttypes.h>

namespace {
constexpr char kTag[] = "WatchMotion";

bool MotionPaused() {
    return LocalRecordingService::GetInstance().GetSnapshot().state != LocalRecordingService::State::kClosed ||
        TemporaryAPTransferService::GetInstance().IsActive() ||
        AudreamPowerManager::GetInstance().State() == AudreamPowerState::kDeepSleepPreparing;
}

}  // namespace

WatchMotionRuntime::WatchMotionRuntime(i2c_master_bus_handle_t bus) : wrist_({
    LoadWatchWristPreferences, SaveWatchWristPreferences,
    []() { AudreamPowerManager::GetInstance().TryWakeDisplayForMotion(); },
}), task_({
    [this, bus]() {
        const bool ready = sensor_.Begin(bus);
        ESP_LOGI(kTag, "QMI8658 initialization: ready=%d", ready);
        return ready;
    },
    [this]() {
        if (!sensor_.Start()) {
            ESP_LOGE(kTag, "QMI8658 pedometer start failed");
            return uint64_t(0);
        }
        ESP_LOGI(kTag, "Pedometer started: generation=%" PRIu64 " rate_hz=112", sensor_.Generation());
        return sensor_.Generation();
    },
    [this](WatchImuReading& reading) { return sensor_.Read(reading); },
    [this]() {
        const bool stopped = sensor_.Stop();
        ESP_LOGI(kTag, "Pedometer stopped: verified=%d", stopped);
        return stopped;
    },
    LoadWatchActivityRecord, SaveWatchActivityRecord,
    []() {
        const auto clock = ReadWatchClock();
        const auto time = GetWatchTimeService().Snapshot(clock);
        const auto calendar = WatchCalendarAt(clock.unix_ms, time.state.timezone);
        return WatchMotionClock{uint64_t(esp_timer_get_time() / 1000),
            calendar.valid ? std::optional<int32_t>(calendar.day) : std::nullopt};
    },
    MotionPaused,
    [this](const WatchImuReading* reading, uint64_t now_ms, bool allowed) { ObserveWrist(reading, now_ms, allowed); },
}) {}

bool WatchMotionRuntime::RequestWrist(uint64_t id, WatchWristAction action, bool enabled) {
    const auto motion = task_.Snapshot();
    const bool cancel = action == WatchWristAction::kCancel || (action == WatchWristAction::kSetEnabled && !enabled);
    if (!cancel && (MotionPaused() || !motion.initialized || motion.activity.status != WatchMotionStatus::kRunning)) return false;
    return wrist_.Request(id, action, enabled);
}

void WatchMotionRuntime::ObserveWrist(const WatchImuReading* reading, uint64_t now_ms, bool allowed) {
    wrist_.Poll(reading, now_ms, allowed && !MotionPaused(), AudreamPowerManager::GetInstance().IsDisplayIdle());
}
