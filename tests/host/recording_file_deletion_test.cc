#include "storage/recording_file_deletion.h"
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace {
struct Fixture {
    std::filesystem::path directory;
    std::string audio, metadata;
    Fixture() {
        char pattern[] = "/tmp/audream-delete-XXXXXX";
        const auto* path = mkdtemp(pattern); assert(path);
        directory = path; audio = (directory / "saved.opus").string(); metadata = audio + ".json";
        Write(audio, 100); Write(metadata, 40);
    }
    ~Fixture() { std::filesystem::remove_all(directory); }
    static void Write(const std::string& path, size_t bytes) {
        std::ofstream file(path, std::ios::binary); file << std::string(bytes, 'a'); assert(file.good());
    }
};
struct Io : RecordingDeleteIo {
    std::string fail_stat, fail_remove, disappear;
    unsigned removes = 0;
    int Stat(const char* path, struct stat* info) override {
        if (path == fail_stat) { errno = EIO; return -1; }
        return RecordingDeleteIo::Stat(path, info);
    }
    int Remove(const char* path) override {
        ++removes;
        if (path == fail_remove) { errno = EIO; return -1; }
        if (path == disappear) RecordingDeleteIo::Remove(path);
        return RecordingDeleteIo::Remove(path);
    }
};
void TestCompleteAndMissingMetadata() {
    for (bool metadata : {true, false}) {
        Fixture f; Io io;
        if (!metadata) std::filesystem::remove(f.metadata);
        errno = ENOENT;
        const auto result = DeleteRecordingFiles(f.audio, 100, false, io);
        assert(result.status == RecordingDeleteStatus::kDeleted && result.audio_absent && result.audio_removed);
        assert(result.removed_bytes == (metadata ? 140 : 100));
        assert(!std::filesystem::exists(f.audio) && !std::filesystem::exists(f.metadata));
    }
}
void TestPreflightCannotDeleteChangedOrUnreadableFiles() {
    Fixture f; Io io;
    auto result = DeleteRecordingFiles(f.audio, 101, false, io);
    assert(result.status == RecordingDeleteStatus::kChanged && io.removes == 0);
    io.fail_stat = f.audio;
    result = DeleteRecordingFiles(f.audio, 100, false, io);
    assert(result.status == RecordingDeleteStatus::kFailed && !result.audio_absent && io.removes == 0);
    io.fail_stat = f.metadata;
    result = DeleteRecordingFiles(f.audio, 100, false, io);
    assert(result.status == RecordingDeleteStatus::kFailed && io.removes == 0);
    assert(std::filesystem::exists(f.audio) && std::filesystem::exists(f.metadata));
}
void TestAudioFailureDoesNotRemoveMetadata() {
    Fixture f; Io io; io.fail_remove = f.audio;
    const auto result = DeleteRecordingFiles(f.audio, 100, false, io);
    assert(result.status == RecordingDeleteStatus::kFailed && !result.audio_absent && !result.audio_removed);
    assert(result.removed_bytes == 0 && io.removes == 1 && std::filesystem::exists(f.metadata));
}
void TestPartialFailureAndCleanup() {
    Fixture f; Io io; io.fail_remove = f.metadata;
    auto result = DeleteRecordingFiles(f.audio, 100, false, io);
    assert(result.status == RecordingDeleteStatus::kPartial && result.audio_absent && result.audio_removed);
    assert(result.removed_bytes == 100 && std::filesystem::exists(f.metadata));
    result = DeleteRecordingFiles(f.audio, 100, true, io);
    assert(result.status == RecordingDeleteStatus::kPartial && !result.audio_removed && result.removed_bytes == 0);
    io.fail_remove.clear();
    result = DeleteRecordingFiles(f.audio, 100, true, io);
    assert(result.status == RecordingDeleteStatus::kDeleted && result.audio_absent && !result.audio_removed);
    assert(result.removed_bytes == 40 && !std::filesystem::exists(f.metadata));
    result = DeleteRecordingFiles(f.audio, 100, true, io);
    assert(result.status == RecordingDeleteStatus::kDeleted && result.removed_bytes == 0);
}
void TestCleanupRefusesRecreatedAudio() {
    Fixture f; Io io; io.fail_remove = f.metadata;
    assert(DeleteRecordingFiles(f.audio, 100, false, io).status == RecordingDeleteStatus::kPartial);
    Fixture::Write(f.audio, 100); io.fail_remove.clear(); const auto removes = io.removes;
    const auto result = DeleteRecordingFiles(f.audio, 100, true, io);
    assert(result.status == RecordingDeleteStatus::kChanged && !result.audio_absent && io.removes == removes);
    assert(std::filesystem::exists(f.audio) && std::filesystem::exists(f.metadata));
}
void TestMissingAudioRequiresExplicitCleanup() {
    Fixture f; Io io; std::filesystem::remove(f.audio);
    const auto result = DeleteRecordingFiles(f.audio, 100, false, io);
    assert(result.status == RecordingDeleteStatus::kNotFound && result.audio_absent && io.removes == 0);
    assert(std::filesystem::exists(f.metadata));
    io.fail_stat = f.metadata;
    const auto retry = DeleteRecordingFiles(f.audio, 100, true, io);
    assert(retry.status == RecordingDeleteStatus::kFailed && retry.audio_absent && io.removes == 0);
}
void TestDirectoryIsNotARecordingFile() {
    Fixture f; Io io;
    std::filesystem::remove(f.metadata); std::filesystem::create_directory(f.metadata);
    assert(DeleteRecordingFiles(f.audio, 100, false, io).status == RecordingDeleteStatus::kFailed);
    assert(io.removes == 0 && std::filesystem::exists(f.audio));
    std::filesystem::remove(f.audio); std::filesystem::create_directory(f.audio);
    assert(DeleteRecordingFiles(f.audio, 100, false, io).status == RecordingDeleteStatus::kChanged);
    assert(io.removes == 0);
}
void TestConcurrentlyMissingMetadataIsNotCountedTwice() {
    Fixture f; Io io; io.disappear = f.metadata;
    const auto result = DeleteRecordingFiles(f.audio, 100, false, io);
    assert(result.status == RecordingDeleteStatus::kDeleted && result.removed_bytes == 100);
}
}
int main() {
    TestCompleteAndMissingMetadata(); TestPreflightCannotDeleteChangedOrUnreadableFiles();
    TestAudioFailureDoesNotRemoveMetadata(); TestPartialFailureAndCleanup(); TestCleanupRefusesRecreatedAudio();
    TestMissingAudioRequiresExplicitCleanup(); TestDirectoryIsNotARecordingFile();
    TestConcurrentlyMissingMetadataIsNotCountedTwice();
    std::cout << "8 recording deletion test groups passed\n";
}
