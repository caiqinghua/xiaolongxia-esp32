#include "system/battery_telemetry_service.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_rtc_time.h"
#include "storage/sd_card_storage.h"
#include "system/device_time_service.h"

namespace {

constexpr char kTag[] = "BatteryTelemetry";
constexpr char kTelemetryDirectory[] = "/sdcard/audream/telemetry/battery";
constexpr char kCurrentFile[] = "/sdcard/audream/telemetry/battery/current.bin";
constexpr char kPreviousFile[] = "/sdcard/audream/telemetry/battery/previous.bin";
constexpr uint8_t kFileHeader[] = {'A', 'D', 'B', 'T', 1, 44, 0, 0};
constexpr size_t kFileHeaderSize = sizeof(kFileHeader);
constexpr size_t kMaxBufferedRecords = 512;
constexpr size_t kMaxSegmentBytes = 4 * 1024 * 1024;
constexpr uint64_t kTimerIntervalUs = 5ULL * 1000ULL * 1000ULL;
constexpr uint64_t kPeriodicIntervalUs = 60ULL * 1000ULL * 1000ULL;
constexpr uint64_t kStartupSampleTimesUs[] = {
    5ULL * 1000ULL * 1000ULL,
    15ULL * 1000ULL * 1000ULL,
    30ULL * 1000ULL * 1000ULL,
};

void AppendUInt16(std::vector<uint8_t>& output, uint16_t value) {
    output.push_back(static_cast<uint8_t>(value & 0xFF));
    output.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void AppendUInt32(std::vector<uint8_t>& output, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }
}

void AppendUInt64(std::vector<uint8_t>& output, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }
}

uint16_t ReadUInt16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
        (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t ReadUInt32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t ReadUInt64(const uint8_t* data) {
    uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<uint64_t>(data[shift / 8]) << shift;
    }
    return value;
}

BatteryTelemetryCursor CursorFor(const BatteryTelemetryRecord& record) {
    return {
        .rtc_epoch_sequence = record.rtc_epoch_sequence,
        .rtc_time_us = record.rtc_time_us,
    };
}

bool CursorLess(
    const BatteryTelemetryCursor& left,
    const BatteryTelemetryCursor& right
) {
    return left.rtc_epoch_sequence < right.rtc_epoch_sequence ||
        (
            left.rtc_epoch_sequence == right.rtc_epoch_sequence &&
            left.rtc_time_us < right.rtc_time_us
        );
}

bool CursorEqual(
    const BatteryTelemetryCursor& left,
    const BatteryTelemetryCursor& right
) {
    return left.rtc_epoch_sequence == right.rtc_epoch_sequence &&
        left.rtc_time_us == right.rtc_time_us;
}

bool CursorIsZero(const BatteryTelemetryCursor& cursor) {
    return cursor.rtc_epoch_sequence == 0 && cursor.rtc_time_us == 0;
}

uint32_t Crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

bool EnsureDirectory(const char* path) {
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return true;
    }
    ESP_LOGE(kTag, "Failed to create telemetry directory %s: %s", path, std::strerror(errno));
    return false;
}

bool EnsureTelemetryDirectory() {
    return EnsureDirectory("/sdcard/audream") &&
        EnsureDirectory("/sdcard/audream/telemetry") &&
        EnsureDirectory(kTelemetryDirectory);
}

bool RemoveIfPresent(const char* path) {
    if (remove(path) == 0 || errno == ENOENT) {
        return true;
    }
    ESP_LOGE(kTag, "Failed to remove battery telemetry file %s: %s", path, std::strerror(errno));
    return false;
}

bool ReadHeader(FILE* file) {
    uint8_t header[kFileHeaderSize] = {};
    return fseek(file, 0, SEEK_SET) == 0 &&
        fread(header, 1, sizeof(header), file) == sizeof(header) &&
        std::memcmp(header, kFileHeader, sizeof(header)) == 0;
}

bool WriteHeader(FILE* file) {
    return fwrite(kFileHeader, 1, sizeof(kFileHeader), file) == sizeof(kFileHeader);
}

uint64_t FileSize(const char* path) {
    struct stat status = {};
    if (stat(path, &status) != 0 || status.st_size < 0) {
        return 0;
    }
    return static_cast<uint64_t>(status.st_size);
}

uint32_t RecordCount(const char* path) {
    const uint64_t size = FileSize(path);
    if (size < kFileHeaderSize) {
        return 0;
    }
    return static_cast<uint32_t>(
        (size - kFileHeaderSize) / BatteryTelemetryService::kEncodedRecordSize
    );
}

bool DecodeRecord(const uint8_t* data, BatteryTelemetryRecord& record) {
    if (data[0] != BatteryTelemetryService::kRecordVersion) {
        return false;
    }
    const uint32_t expected_crc = ReadUInt32(
        &data[BatteryTelemetryService::kEncodedRecordSize - sizeof(uint32_t)]
    );
    const uint32_t actual_crc = Crc32(
        data,
        BatteryTelemetryService::kEncodedRecordSize - sizeof(uint32_t)
    );
    if (expected_crc != actual_crc) {
        return false;
    }
    record.version = data[0];
    record.event = static_cast<BatteryTelemetryEvent>(data[1]);
    record.flags = data[2];
    record.battery_percent = data[3];
    record.battery_voltage_mv = ReadUInt16(&data[4]);
    record.status1 = data[6];
    record.status2 = data[7];
    record.unix_time_ms = ReadUInt64(&data[8]);
    record.uptime_ms = ReadUInt64(&data[16]);
    record.rtc_epoch_sequence = ReadUInt64(&data[24]);
    record.rtc_time_us = ReadUInt64(&data[32]);
    return true;
}

bool ReadRecordAt(FILE* file, uint32_t index, BatteryTelemetryRecord& record) {
    const uint64_t offset =
        kFileHeaderSize +
        static_cast<uint64_t>(index) * BatteryTelemetryService::kEncodedRecordSize;
    if (fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
        return false;
    }
    uint8_t encoded[BatteryTelemetryService::kEncodedRecordSize] = {};
    return fread(encoded, 1, sizeof(encoded), file) == sizeof(encoded) &&
        DecodeRecord(encoded, record);
}

bool ReadFileBoundary(
    const char* path,
    uint32_t& count,
    BatteryTelemetryRecord& first,
    BatteryTelemetryRecord& last
) {
    count = RecordCount(path);
    if (count == 0) {
        return false;
    }
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        count = 0;
        return false;
    }
    const bool valid = ReadHeader(file) &&
        ReadRecordAt(file, 0, first) &&
        ReadRecordAt(file, count - 1, last);
    fclose(file);
    if (!valid) {
        count = 0;
    }
    return valid;
}

void ReadFileAfter(
    const char* path,
    const BatteryTelemetryCursor& cursor,
    size_t max_count,
    std::vector<BatteryTelemetryRecord>& output
) {
    const uint32_t count = RecordCount(path);
    if (count == 0 || output.size() >= max_count) {
        return;
    }
    FILE* file = fopen(path, "rb");
    if (file == nullptr || !ReadHeader(file)) {
        if (file != nullptr) {
            fclose(file);
        }
        return;
    }

    uint32_t low = 0;
    uint32_t high = count;
    while (low < high) {
        const uint32_t middle = low + (high - low) / 2;
        BatteryTelemetryRecord record;
        if (!ReadRecordAt(file, middle, record)) {
            fclose(file);
            return;
        }
        if (!CursorLess(cursor, CursorFor(record))) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    for (uint32_t index = low; index < count && output.size() < max_count; ++index) {
        BatteryTelemetryRecord record;
        if (ReadRecordAt(file, index, record)) {
            output.push_back(record);
        }
    }
    fclose(file);
}

}  // namespace

BatteryTelemetryService& BatteryTelemetryService::GetInstance() {
    static BatteryTelemetryService instance;
    return instance;
}

void BatteryTelemetryService::Start(
    std::function<bool(BatteryTelemetrySnapshot&)> snapshot_provider
) {
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (enabled_) {
            snapshot_provider_ = std::move(snapshot_provider);
            return;
        }
        snapshot_provider_ = std::move(snapshot_provider);
        enabled_ = snapshot_provider_ != nullptr;
        started_at_us_ = esp_timer_get_time();
        last_periodic_sample_us_ = started_at_us_;
        startup_sample_index_ = 0;
        enabled = enabled_;
    }
    if (!enabled) {
        return;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = TimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "battery_telemetry",
        .skip_unhandled_events = true,
    };
    const esp_err_t create_result = esp_timer_create(&timer_args, &timer_);
    if (create_result != ESP_OK) {
        ESP_LOGE(kTag, "Failed to create battery telemetry timer: %s", esp_err_to_name(create_result));
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = false;
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_start_periodic(timer_, kTimerIntervalUs));
    RecordEvent(BatteryTelemetryEvent::kBoot);
    ESP_LOGI(kTag, "Battery telemetry started");
}

void BatteryTelemetryService::Stop() {
    esp_timer_handle_t timer = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = false;
        timer = timer_;
        timer_ = nullptr;
    }
    if (timer != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_stop(timer));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_delete(timer));
    }
}

bool BatteryTelemetryService::IsEnabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

void BatteryTelemetryService::TimerCallback(void* arg) {
    auto* service = static_cast<BatteryTelemetryService*>(arg);
    if (service != nullptr) {
        service->HandleTimer();
    }
}

void BatteryTelemetryService::HandleTimer() {
    const uint64_t now_us = esp_timer_get_time();
    BatteryTelemetryEvent event = BatteryTelemetryEvent::kPeriodic;
    bool should_record = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) {
            return;
        }
        const uint64_t elapsed_us = now_us - started_at_us_;
        if (startup_sample_index_ < std::size(kStartupSampleTimesUs) &&
            elapsed_us >= kStartupSampleTimesUs[startup_sample_index_]) {
            startup_sample_index_++;
            event = BatteryTelemetryEvent::kStartupSample;
            should_record = true;
        } else if (now_us - last_periodic_sample_us_ >= kPeriodicIntervalUs) {
            last_periodic_sample_us_ = now_us;
            event = BatteryTelemetryEvent::kPeriodic;
            should_record = true;
        }
    }
    if (should_record) {
        RecordEvent(event);
    }
}

bool BatteryTelemetryService::Capture(
    BatteryTelemetryEvent event,
    BatteryTelemetryRecord& record
) {
    std::function<bool(BatteryTelemetrySnapshot&)> provider;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_ || !snapshot_provider_) {
            return false;
        }
        provider = snapshot_provider_;
    }
    BatteryTelemetrySnapshot snapshot;
    if (!provider(snapshot)) {
        ESP_LOGW(kTag, "Battery telemetry snapshot was unavailable");
        return false;
    }

    auto& time_service = DeviceTimeService::GetInstance();
    record.event = event;
    record.flags = snapshot.flags;
    record.battery_percent = std::min<uint8_t>(snapshot.battery_percent, 100);
    record.battery_voltage_mv = snapshot.battery_voltage_mv;
    record.status1 = snapshot.status1;
    record.status2 = snapshot.status2;
    record.unix_time_ms = time_service.CurrentUnixTimeMs();
    record.uptime_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    record.rtc_epoch_sequence = time_service.RtcEpochSeq();
    record.rtc_time_us = esp_rtc_get_time_us();
    return true;
}

void BatteryTelemetryService::NormalizeCursorLocked(BatteryTelemetryRecord& record) {
    auto cursor = CursorFor(record);
    if (CursorLess(cursor, last_cursor_) || CursorEqual(cursor, last_cursor_)) {
        record.rtc_epoch_sequence = last_cursor_.rtc_epoch_sequence;
        record.rtc_time_us = last_cursor_.rtc_time_us + 1;
        cursor = CursorFor(record);
    }
    last_cursor_ = cursor;
}

void BatteryTelemetryService::RecordEvent(BatteryTelemetryEvent event) {
    BatteryTelemetryRecord record;
    if (!Capture(event, record)) {
        return;
    }
    bool should_flush = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        NormalizeCursorLocked(record);
        buffered_records_.push_back(record);
        should_flush = buffered_records_.size() >= kMaxBufferedRecords;
    }
    if (should_flush) {
        std::lock_guard<std::mutex> lock(mutex_);
        FlushLocked();
    }
}

bool BatteryTelemetryService::FlushBeforeSleep() {
    RecordEvent(BatteryTelemetryEvent::kPreSleep);
    std::lock_guard<std::mutex> lock(mutex_);
    return FlushLocked();
}

bool BatteryTelemetryService::FlushLocked() {
    if (buffered_records_.empty()) {
        return true;
    }
    if (!SdCardStorage::GetInstance().IsMounted() || !EnsureTelemetryDirectory()) {
        return false;
    }

    const size_t encoded_size = buffered_records_.size() * kEncodedRecordSize;
    uint64_t current_size = FileSize(kCurrentFile);
    if (current_size > 0) {
        FILE* current = fopen(kCurrentFile, "rb");
        const bool valid_header = current != nullptr && ReadHeader(current);
        if (current != nullptr) {
            fclose(current);
        }
        if (!valid_header) {
            ESP_LOGW(kTag, "Discarding incompatible battery telemetry segment");
            if (!RemoveIfPresent(kCurrentFile)) {
                return false;
            }
            current_size = 0;
        }
    }
    if (current_size >= kFileHeaderSize &&
        current_size + encoded_size > kMaxSegmentBytes) {
        remove(kPreviousFile);
        if (rename(kCurrentFile, kPreviousFile) != 0) {
            ESP_LOGE(kTag, "Failed to rotate battery telemetry: %s", std::strerror(errno));
            return false;
        }
        current_size = 0;
    }

    FILE* file = fopen(kCurrentFile, current_size == 0 ? "wb" : "ab");
    if (file == nullptr) {
        ESP_LOGE(kTag, "Failed to open battery telemetry file: %s", std::strerror(errno));
        return false;
    }
    bool success = true;
    if (current_size == 0) {
        success = WriteHeader(file);
    }
    std::vector<uint8_t> encoded;
    encoded.reserve(encoded_size);
    if (success) {
        for (const auto& record : buffered_records_) {
            AppendEncodedRecord(encoded, record);
        }
        success = fwrite(encoded.data(), 1, encoded.size(), file) == encoded.size();
    }
    if (success) {
        success = fflush(file) == 0 && fsync(fileno(file)) == 0;
    }
    fclose(file);
    if (!success) {
        ESP_LOGE(kTag, "Failed to persist battery telemetry");
        return false;
    }
    ESP_LOGI(
        kTag,
        "Persisted %u battery telemetry records in one SD batch",
        static_cast<unsigned>(buffered_records_.size())
    );
    buffered_records_.clear();
    return true;
}

BatteryTelemetryInfo BatteryTelemetryService::GetPersistedInfoLocked() {
    BatteryTelemetryInfo info;
    for (const char* path : {kPreviousFile, kCurrentFile}) {
        uint32_t count = 0;
        BatteryTelemetryRecord first;
        BatteryTelemetryRecord last;
        if (!ReadFileBoundary(path, count, first, last)) {
            continue;
        }
        info.record_count += count;
        const auto first_cursor = CursorFor(first);
        const auto last_cursor = CursorFor(last);
        if (CursorIsZero(info.oldest_cursor) || CursorLess(first_cursor, info.oldest_cursor)) {
            info.oldest_cursor = first_cursor;
        }
        if (CursorLess(info.newest_cursor, last_cursor)) {
            info.newest_cursor = last_cursor;
        }
    }
    return info;
}

BatteryTelemetryInfo BatteryTelemetryService::GetInfo() {
    std::lock_guard<std::mutex> lock(mutex_);
    BatteryTelemetryInfo info = GetPersistedInfoLocked();
    for (const auto& record : buffered_records_) {
        const auto cursor = CursorFor(record);
        info.record_count++;
        if (CursorIsZero(info.oldest_cursor) || CursorLess(cursor, info.oldest_cursor)) {
            info.oldest_cursor = cursor;
        }
        if (CursorLess(info.newest_cursor, cursor)) {
            info.newest_cursor = cursor;
        }
    }
    return info;
}

std::vector<BatteryTelemetryRecord> BatteryTelemetryService::ReadPersistedAfterLocked(
    const BatteryTelemetryCursor& cursor,
    size_t max_count
) {
    std::vector<BatteryTelemetryRecord> records;
    records.reserve(max_count);
    ReadFileAfter(kPreviousFile, cursor, max_count, records);
    ReadFileAfter(kCurrentFile, cursor, max_count, records);
    return records;
}

std::vector<BatteryTelemetryRecord> BatteryTelemetryService::ReadAfter(
    const BatteryTelemetryCursor& cursor,
    size_t max_count
) {
    max_count = std::clamp<size_t>(max_count, 1, 4);
    std::lock_guard<std::mutex> lock(mutex_);
    auto records = ReadPersistedAfterLocked(cursor, max_count);
    for (const auto& record : buffered_records_) {
        if (records.size() >= max_count) {
            break;
        }
        if (CursorLess(cursor, CursorFor(record))) {
            records.push_back(record);
        }
    }
    std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
        return CursorLess(CursorFor(left), CursorFor(right));
    });
    if (records.size() > max_count) {
        records.resize(max_count);
    }
    return records;
}

bool BatteryTelemetryService::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    buffered_records_.clear();
    const bool current_removed = RemoveIfPresent(kCurrentFile);
    const bool previous_removed = RemoveIfPresent(kPreviousFile);
    return current_removed && previous_removed;
}

void BatteryTelemetryService::AppendEncodedRecord(
    std::vector<uint8_t>& output,
    const BatteryTelemetryRecord& record
) {
    const size_t start = output.size();
    output.push_back(kRecordVersion);
    output.push_back(static_cast<uint8_t>(record.event));
    output.push_back(record.flags);
    output.push_back(record.battery_percent);
    AppendUInt16(output, record.battery_voltage_mv);
    output.push_back(record.status1);
    output.push_back(record.status2);
    AppendUInt64(output, record.unix_time_ms);
    AppendUInt64(output, record.uptime_ms);
    AppendUInt64(output, record.rtc_epoch_sequence);
    AppendUInt64(output, record.rtc_time_us);
    const uint32_t crc = Crc32(&output[start], output.size() - start);
    AppendUInt32(output, crc);
}
