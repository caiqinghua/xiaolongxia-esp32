#include "watch_recording_library.h"
#include "cJSON.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <sys/stat.h>

namespace {
bool Newer(const WatchRecordingEntry& left, const WatchRecordingEntry& right) {
    return left.sort_key == right.sort_key ? left.id > right.id : left.sort_key > right.sort_key;
}
bool SafeId(const std::string& id) {
    return !id.empty() && id.size() <= 96 && std::all_of(id.begin(), id.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    });
}
bool BoundedDepth(const std::vector<char>& text) {
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (char ch : text) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '[' || ch == '{') { if (++depth > 12) return false; }
        else if (ch == ']' || ch == '}') { if (!depth) return false; --depth; }
    }
    return !quoted && depth == 0;
}
bool Number(cJSON* root, const char* key, uint64_t maximum, uint64_t& value) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || item->valuedouble < 0 ||
        item->valuedouble > static_cast<double>(maximum) || std::floor(item->valuedouble) != item->valuedouble) return false;
    value = static_cast<uint64_t>(item->valuedouble);
    return true;
}
void ReadMetadata(const std::string& path, WatchRecordingEntry& entry) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        static_cast<uint64_t>(st.st_size) > WatchRecordingLibrary::kMetadataLimit) return;
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(path.c_str(), "rb"), fclose);
    if (!file) return;
    std::vector<char> text(static_cast<size_t>(st.st_size) + 1, '\0');
    if (fread(text.data(), 1, text.size() - 1, file.get()) != text.size() - 1 ||
        std::memchr(text.data(), '\0', text.size() - 1) || !BoundedDepth(text)) return;
    const char* end = nullptr;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        cJSON_ParseWithLengthOpts(text.data(), text.size(), &end, true), cJSON_Delete);
    if (!root || !cJSON_IsObject(root.get()) || end != text.data() + text.size() - 1) return;
    const auto* id = cJSON_GetObjectItemCaseSensitive(root.get(), "recording_id");
    uint64_t version = 0, duration = 0, size = 0, timestamp = 0, epoch = 0, rtc = 0;
    constexpr uint64_t kExactInteger = 9007199254740991ULL;
    if (!Number(root.get(), "schema_version", 1, version) || version != 1 ||
        !cJSON_IsString(id) || entry.id != id->valuestring ||
        !Number(root.get(), "size_bytes", kExactInteger, size) || size != entry.size_bytes) return;
    const auto* complete = cJSON_GetObjectItemCaseSensitive(root.get(), "recording_complete");
    entry.incomplete = cJSON_IsFalse(complete);
    if (!Number(root.get(), "duration_ms", UINT32_MAX, duration) || !duration) return;
    entry.duration_ms = static_cast<uint32_t>(duration);
    entry.metadata_available = true;
    Number(root.get(), "started_at_unix_ms", kExactInteger, timestamp);
    entry.started_at_unix_ms = timestamp;
    if (timestamp) entry.sort_key = timestamp;
    else {
        constexpr uint64_t kStride = 1000000000000ULL;
        Number(root.get(), "rtc_epoch_seq", (UINT64_MAX - (kStride - 1)) / kStride, epoch);
        Number(root.get(), "started_at_rtc_ms", kExactInteger, rtc);
        if (epoch && rtc) entry.sort_key = epoch * kStride + std::min(rtc, kStride - 1);
        else if (rtc) entry.sort_key = rtc;
    }
}
}

WatchRecordingLibrary::~WatchRecordingLibrary() { Close(); }
void WatchRecordingLibrary::Forget(const std::string& recording_id) {
    snapshot_.entries.erase(std::remove_if(snapshot_.entries.begin(), snapshot_.entries.end(),
        [&](const auto& entry) { return entry.id == recording_id; }), snapshot_.entries.end());
    ++snapshot_.revision;
}
void WatchRecordingLibrary::Close() {
    if (directory_) closedir(directory_);
    directory_ = nullptr;
    pending_.clear();
}
void WatchRecordingLibrary::Cancel(WatchLibraryStatus status) {
    Close();
    snapshot_.status = status;
    ++snapshot_.revision;
}
bool WatchRecordingLibrary::Begin(const std::string& directory, const std::string& excluded_id) {
    return BeginPage(directory, excluded_id, Direction::kNewest, std::nullopt);
}
bool WatchRecordingLibrary::BeginOlder() {
    if (snapshot_.status != WatchLibraryStatus::kReady || !snapshot_.has_older) return false;
    const bool empty = snapshot_.entries.empty();
    auto anchor = empty ? anchor_ : std::optional<WatchRecordingEntry>(snapshot_.entries.back());
    if (!anchor) return false;
    return BeginPage(path_, excluded_id_, Direction::kOlder, std::move(anchor), empty);
}
bool WatchRecordingLibrary::BeginNewer() {
    if (snapshot_.status != WatchLibraryStatus::kReady || !snapshot_.has_newer) return false;
    const bool empty = snapshot_.entries.empty();
    auto anchor = empty ? anchor_ : std::optional<WatchRecordingEntry>(snapshot_.entries.front());
    if (!anchor) return false;
    return BeginPage(path_, excluded_id_, Direction::kNewer, std::move(anchor), empty);
}
bool WatchRecordingLibrary::BeginPage(const std::string& directory, const std::string& excluded_id,
    Direction direction, std::optional<WatchRecordingEntry> anchor, bool include_anchor) {
    Close();
    path_ = directory;
    excluded_id_ = excluded_id;
    direction_ = direction;
    anchor_ = std::move(anchor);
    include_anchor_ = include_anchor;
    overflow_ = skipped_ = false;
    snapshot_.entries.clear();
    snapshot_.has_newer = snapshot_.has_older = false;
    ++snapshot_.revision;
    directory_ = opendir(path_.c_str());
    snapshot_.status = directory_ ? WatchLibraryStatus::kLoading : WatchLibraryStatus::kUnavailable;
    return directory_ != nullptr;
}
bool WatchRecordingLibrary::Step(size_t max_entries) {
    if (!Loading() || !directory_) return false;
    // A caller cannot accidentally turn this into an unbounded directory scan.
    max_entries = std::min<size_t>(max_entries, 8);
    for (size_t index = 0; index < max_entries; ++index) {
        errno = 0;
        const auto* item = readdir(directory_);
        if (!item) {
            if (errno) { Cancel(WatchLibraryStatus::kUnavailable); return false; }
            snapshot_.entries = std::move(pending_);
            snapshot_.has_newer = direction_ == Direction::kNewer ? overflow_ :
                direction_ == Direction::kOlder && skipped_;
            snapshot_.has_older = direction_ == Direction::kNewer ? skipped_ : overflow_;
            Close();
            snapshot_.status = WatchLibraryStatus::kReady;
            ++snapshot_.revision;
            return false;
        }
        Add(item->d_name);
    }
    return true;
}
void WatchRecordingLibrary::Add(const char* name) {
    const std::string filename(name);
    const bool metadata = filename.size() > 10 && filename.compare(filename.size() - 10, 10, ".opus.json") == 0;
    if (!metadata && (filename.size() <= 5 || filename.compare(filename.size() - 5, 5, ".opus") != 0)) return;
    WatchRecordingEntry entry;
    entry.id = filename.substr(0, filename.size() - (metadata ? 10 : 5));
    if (!SafeId(entry.id) || entry.id == excluded_id_) return;
    const std::string path = path_ + "/" + filename;
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (!metadata && st.st_size == 0)) return;
    if (metadata) {
        struct stat audio{};
        // Only a confirmed missing audio path is an orphan, never an I/O error or an empty audio file.
        if (stat((path_ + "/" + entry.id + ".opus").c_str(), &audio) == 0 || errno != ENOENT) return;
        entry.audio_available = false;
    }
    entry.file_name = filename;
    entry.size_bytes = static_cast<uint64_t>(st.st_size);
    if (st.st_mtime > 0) entry.sort_key = std::min<uint64_t>(static_cast<uint64_t>(st.st_mtime), UINT64_MAX / 1000) * 1000;
    if (!metadata) ReadMetadata(path + ".json", entry);
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const auto& candidate) {
        return candidate.id == entry.id;
    }), pending_.end());
    if (anchor_) {
        const bool equal = entry.id == anchor_->id && entry.sort_key == anchor_->sort_key;
        const bool matches = (include_anchor_ && equal) ||
            (direction_ == Direction::kOlder ? Newer(*anchor_, entry) : Newer(entry, *anchor_));
        if (!matches) { skipped_ = true; return; }
    }
    const auto before = std::lower_bound(pending_.begin(), pending_.end(), entry,
        Newer);
    pending_.insert(before, std::move(entry));
    if (pending_.size() > kMaxEntries) {
        overflow_ = true;
        // Reverse navigation retains the nearest newer page, not the newest page overall.
        if (direction_ == Direction::kNewer) pending_.erase(pending_.begin());
        else pending_.pop_back();
    }
}
