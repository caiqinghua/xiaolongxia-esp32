#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

enum class StorageStatusErrorCode : uint32_t {
    kNone = 0,
    kPinsNotConfigured = 1,
    kSpiBusInitFailed = 2,
    kMountFailed = 3,
    kDirectoryCreateFailed = 4,
    kFreeSpaceReadFailed = 5,
};

struct StorageStatus {
    bool mounted = false;
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    uint64_t recording_bytes = 0;
    uint32_t estimated_remaining_minutes = 0;
    StorageStatusErrorCode error_code = StorageStatusErrorCode::kNone;
};

struct StoredRecordingInfo {
    std::string recording_id;
    std::string file_name;
    uint32_t duration_ms = 0;
    uint32_t size_bytes = 0;
    uint64_t started_at_unix_ms = 0;
    uint64_t started_at_uptime_ms = 0;
    uint64_t started_at_rtc_ms = 0;
};

class SdCardStorage {
public:
    static SdCardStorage& GetInstance();

    void SetInitializationCompleteCallback(std::function<void(const StorageStatus&)> callback);
    void StartInitializationTask();
    bool Mount();
    bool IsMounted() const;
    void PrepareForDeepSleep();
    StorageStatus ProbeAndMountStatus();
    StorageStatus GetStatus();
    std::string RecordingsDirectory() const;
    bool EnsureRecordingsDirectory();
    bool SetRecordingMtime(const std::string& recording_id, uint64_t started_at_unix_ms);
    std::vector<StoredRecordingInfo> ListRecordings(
        size_t offset,
        size_t max_count,
        size_t* total_count = nullptr
    );
    bool GetRecordingFilePath(
        const std::string& recording_id,
        std::string& file_path,
        uint32_t& total_size
    );
    bool DeleteRecording(const std::string& recording_id);
    bool ReadRecordingChunk(
        const std::string& recording_id,
        uint32_t offset,
        size_t max_length,
        std::vector<uint8_t>& output,
        uint32_t& total_size
    );
    size_t BackfillUnsynchronizedRecordingTimes(
        uint64_t synced_unix_time_ms,
        uint64_t synced_rtc_time_ms,
        uint64_t rtc_epoch_seq
    );

private:
    SdCardStorage() = default;

    mutable std::recursive_mutex mutex_;
    bool initialization_task_started_ = false;
    bool mounted_ = false;
    bool ready_ = false;
    std::function<void(const StorageStatus&)> initialization_complete_callback_;
    StorageStatus cached_status_;
    StorageStatusErrorCode last_error_code_ = StorageStatusErrorCode::kNone;
};
