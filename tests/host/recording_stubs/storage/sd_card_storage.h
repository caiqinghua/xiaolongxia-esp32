#pragma once
#include <cstdint>
#include <string>
struct StorageStatus { bool mounted = true; uint32_t error_code = 0; };
class SdCardStorage {
public:
    static SdCardStorage& GetInstance() { static SdCardStorage value; return value; }
    std::string directory;
    StorageStatus status;
    StorageStatus GetStatus() const { return status; }
    std::string RecordingsDirectory() const { return directory; }
    bool SetRecordingMtime(const std::string&, uint64_t) { return true; }
};
