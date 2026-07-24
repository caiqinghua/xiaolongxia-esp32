#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "esp_timer.h"

enum class BatteryTelemetryEvent : uint8_t {
    kBoot = 1,
    kStartup5Seconds = 2,
    kStartup15Seconds = 3,
    kStartup30Seconds = 4,
    kPeriodic = 5,
    kUsbConnected = 6,
    kUsbDisconnected = 7,
    kRecordingStarted = 8,
    kRecordingPaused = 9,
    kRecordingResumed = 10,
    kRecordingStopped = 11,
    kTransferStarted = 12,
    kTransferStopped = 13,
    kPreSleep = 14,
};

enum BatteryTelemetryFlag : uint8_t {
    kBatteryTelemetryCharging = 1 << 0,
    kBatteryTelemetryDischarging = 1 << 1,
    kBatteryTelemetryUsbPowered = 1 << 2,
    kBatteryTelemetryRecording = 1 << 3,
    kBatteryTelemetryRecordingPaused = 1 << 4,
    kBatteryTelemetryTransferActive = 1 << 5,
};

struct BatteryTelemetrySnapshot {
    uint8_t battery_percent = 0;
    uint16_t battery_voltage_mv = 0;
    uint8_t status1 = 0;
    uint8_t status2 = 0;
    uint8_t flags = 0;
};

struct BatteryTelemetryCursor {
    uint64_t rtc_epoch_sequence = 0;
    uint64_t rtc_time_us = 0;
};

struct BatteryTelemetryRecord {
    uint8_t version = 2;
    BatteryTelemetryEvent event = BatteryTelemetryEvent::kPeriodic;
    uint8_t flags = 0;
    uint8_t battery_percent = 0;
    uint16_t battery_voltage_mv = 0;
    uint8_t status1 = 0;
    uint8_t status2 = 0;
    uint64_t unix_time_ms = 0;
    uint64_t uptime_ms = 0;
    uint64_t rtc_epoch_sequence = 0;
    uint64_t rtc_time_us = 0;
};

struct BatteryTelemetryInfo {
    uint32_t record_count = 0;
    BatteryTelemetryCursor oldest_cursor;
    BatteryTelemetryCursor newest_cursor;
};

class BatteryTelemetryService {
public:
    static constexpr uint8_t kRecordVersion = 2;
    static constexpr size_t kEncodedRecordSize = 44;

    static BatteryTelemetryService& GetInstance();

    void Start(std::function<bool(BatteryTelemetrySnapshot&)> snapshot_provider);
    void Stop();
    bool IsEnabled() const;
    void RecordEvent(BatteryTelemetryEvent event);
    bool FlushBeforeSleep();

    BatteryTelemetryInfo GetInfo();
    std::vector<BatteryTelemetryRecord> ReadAfter(
        const BatteryTelemetryCursor& cursor,
        size_t max_count
    );
    bool Clear();

    static void AppendEncodedRecord(
        std::vector<uint8_t>& output,
        const BatteryTelemetryRecord& record
    );

    BatteryTelemetryService(const BatteryTelemetryService&) = delete;
    BatteryTelemetryService& operator=(const BatteryTelemetryService&) = delete;

private:
    BatteryTelemetryService() = default;

    static void TimerCallback(void* arg);
    void HandleTimer();
    bool Capture(BatteryTelemetryEvent event, BatteryTelemetryRecord& record);
    void NormalizeCursorLocked(BatteryTelemetryRecord& record);
    bool FlushLocked();
    BatteryTelemetryInfo GetPersistedInfoLocked();
    std::vector<BatteryTelemetryRecord> ReadPersistedAfterLocked(
        const BatteryTelemetryCursor& cursor,
        size_t max_count
    );

    mutable std::mutex mutex_;
    std::function<bool(BatteryTelemetrySnapshot&)> snapshot_provider_;
    std::vector<BatteryTelemetryRecord> buffered_records_;
    esp_timer_handle_t timer_ = nullptr;
    bool enabled_ = false;
    BatteryTelemetryCursor last_cursor_;
    uint64_t started_at_us_ = 0;
    uint64_t last_periodic_sample_us_ = 0;
    size_t startup_sample_index_ = 0;
};
