#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct StorageStatus {
    bool mounted = false;
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    uint64_t recording_bytes = 0;
    uint32_t estimated_remaining_minutes = 0;
};

struct StoredRecordingInfo {
    std::string recording_id;
    std::string file_name;
    uint32_t duration_ms = 0;
    uint32_t size_bytes = 0;
};

class SdCardStorage {
public:
    static SdCardStorage& GetInstance();

    bool Mount();
    bool IsMounted() const;
    StorageStatus GetStatus();
    std::string RecordingsDirectory() const;
    bool EnsureRecordingsDirectory();
    std::vector<StoredRecordingInfo> ListRecordings(size_t max_count);
    bool ReadRecordingChunk(
        const std::string& recording_id,
        uint32_t offset,
        size_t max_length,
        std::vector<uint8_t>& output,
        uint32_t& total_size
    );

private:
    SdCardStorage() = default;

    bool mounted_ = false;
};
