#include "recording_file_deletion.h"
#include <cerrno>
#include <unistd.h>

int RecordingDeleteIo::Stat(const char* path, struct stat* info) { return stat(path, info); }
int RecordingDeleteIo::Remove(const char* path) { return unlink(path); }

RecordingDeleteResult DeleteRecordingFiles(const std::string& audio_path, uint64_t expected_bytes,
    bool metadata_only, RecordingDeleteIo& io) {
    RecordingDeleteResult result;
    struct stat audio{}, metadata{};
    const int audio_status = io.Stat(audio_path.c_str(), &audio);
    if (audio_status != 0 && errno != ENOENT) return result;
    const bool audio_exists = audio_status == 0;
    result.audio_absent = !audio_exists;
    if (!audio_exists && !metadata_only) {
        result.status = RecordingDeleteStatus::kNotFound;
        return result;
    }
    if (audio_exists && (metadata_only || !S_ISREG(audio.st_mode) || audio.st_size < 0 ||
        (expected_bytes && static_cast<uint64_t>(audio.st_size) != expected_bytes))) {
        result.status = RecordingDeleteStatus::kChanged;
        return result;
    }
    const auto metadata_path = audio_path + ".json";
    const int metadata_status = io.Stat(metadata_path.c_str(), &metadata);
    if (metadata_status != 0 && errno != ENOENT) return result;
    const bool metadata_exists = metadata_status == 0;
    if (metadata_exists && (!S_ISREG(metadata.st_mode) || metadata.st_size < 0)) return result;
    if (audio_exists) {
        if (io.Remove(audio_path.c_str()) != 0) return result;
        result.audio_removed = result.audio_absent = true;
        result.removed_bytes = static_cast<uint64_t>(audio.st_size);
    }
    if (metadata_exists) {
        const int removed = io.Remove(metadata_path.c_str());
        if (removed != 0 && errno != ENOENT) {
            result.status = RecordingDeleteStatus::kPartial;
            return result;
        }
        // Only this invocation's successful unlink contributes to cached free space.
        if (removed == 0) result.removed_bytes += static_cast<uint64_t>(metadata.st_size);
    }
    result.status = RecordingDeleteStatus::kDeleted;
    return result;
}
