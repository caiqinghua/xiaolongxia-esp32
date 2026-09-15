#pragma once

#include <cstdint>
#include <string>
#include <sys/stat.h>

enum class RecordingDeleteStatus { kNone, kDeleted, kNotFound, kChanged, kFailed, kPartial };

struct RecordingDeleteResult {
    RecordingDeleteStatus status = RecordingDeleteStatus::kFailed;
    uint64_t removed_bytes = 0;
    bool audio_removed = false;
    bool audio_absent = false;
};

class RecordingDeleteIo {
public:
    virtual ~RecordingDeleteIo() = default;
    virtual int Stat(const char* path, struct stat* info);
    virtual int Remove(const char* path);
};

// The caller owns storage and excludes capture/transfer for the entire operation.
// Cleanup retries must not remove an audio file that reappeared after partial deletion.
RecordingDeleteResult DeleteRecordingFiles(const std::string& audio_path, uint64_t expected_bytes,
    bool metadata_only, RecordingDeleteIo& io);
