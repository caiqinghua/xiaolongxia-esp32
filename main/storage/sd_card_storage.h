#pragma once

#include <cstdint>
#include <string>

struct StorageStatus {
    bool mounted = false;
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    uint64_t recording_bytes = 0;
    uint32_t estimated_remaining_minutes = 0;
};

class SdCardStorage {
public:
    static SdCardStorage& GetInstance();

    bool Mount();
    bool IsMounted() const;
    StorageStatus GetStatus();
    std::string RecordingsDirectory() const;
    bool EnsureRecordingsDirectory();

private:
    SdCardStorage() = default;

    bool mounted_ = false;
};
