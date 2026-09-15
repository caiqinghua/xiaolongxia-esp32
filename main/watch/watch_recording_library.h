#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <dirent.h>
#include <optional>
#include "storage/recording_file_deletion.h"

struct WatchRecordingDeleteSnapshot {
    uint64_t request = 0;
    std::string recording_id;
    RecordingDeleteResult result;
    bool cleanup_available = false;
};

struct WatchRecordingEntry {
    std::string id;
    std::string file_name;
    uint64_t size_bytes = 0;
    uint64_t started_at_unix_ms = 0;
    uint64_t sort_key = 0;
    uint32_t duration_ms = 0;
    bool metadata_available = false;
    bool incomplete = false;
    bool audio_available = true;
};

enum class WatchLibraryStatus { kUnloaded, kLoading, kReady, kUnavailable, kInterrupted };
struct WatchRecordingLibrarySnapshot {
    WatchLibraryStatus status = WatchLibraryStatus::kUnloaded;
    uint64_t revision = 0;
    std::vector<WatchRecordingEntry> entries;
    bool has_older = false;
    bool has_newer = false;
};

// Owner-task incremental, read-only index. Each step bounds work and memory;
// callers retain a storage/sleep lease until completion or cancellation.
class WatchRecordingLibrary {
public:
    static constexpr size_t kMaxEntries = 20;
    static constexpr size_t kMetadataLimit = 32768;
    ~WatchRecordingLibrary();
    WatchRecordingLibrary() = default;
    WatchRecordingLibrary(const WatchRecordingLibrary&) = delete;
    WatchRecordingLibrary& operator=(const WatchRecordingLibrary&) = delete;
    bool Begin(const std::string& directory, const std::string& excluded_id = {});
    bool BeginOlder();
    bool BeginNewer();
    void Forget(const std::string& recording_id);
    bool Step(size_t max_entries = 4);
    void Cancel(WatchLibraryStatus status = WatchLibraryStatus::kInterrupted);
    bool Loading() const { return snapshot_.status == WatchLibraryStatus::kLoading; }
    const WatchRecordingLibrarySnapshot& Snapshot() const { return snapshot_; }
private:
    DIR* directory_ = nullptr;
    std::string path_;
    std::string excluded_id_;
    std::vector<WatchRecordingEntry> pending_;
    WatchRecordingLibrarySnapshot snapshot_;
    enum class Direction { kNewest, kOlder, kNewer };
    Direction direction_ = Direction::kNewest;
    std::optional<WatchRecordingEntry> anchor_;
    bool include_anchor_ = false;
    bool overflow_ = false;
    bool skipped_ = false;
    bool BeginPage(const std::string& directory, const std::string& excluded_id,
        Direction direction, std::optional<WatchRecordingEntry> anchor, bool include_anchor = false);
    void Close();
    void Add(const char* name);
};
