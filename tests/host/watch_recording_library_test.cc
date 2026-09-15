#include "watch/watch_recording_library.h"
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <set>
#include <unistd.h>

namespace {
struct Fixture {
    std::filesystem::path path;
    Fixture() {
        char pattern[] = "/tmp/audream-catalog-XXXXXX";
        const auto* result = mkdtemp(pattern);
        assert(result);
        path = result;
    }
    ~Fixture() { std::filesystem::remove_all(path); }
    void Write(const std::string& name, const std::string& content) {
        std::ofstream file(path / name, std::ios::binary);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        assert(file.good());
    }
    void Recording(const std::string& id, uint64_t timestamp, const std::string& extra = "") {
        Write(id + ".opus", std::string(32, 'a'));
        Write(id + ".opus.json", "{\"schema_version\":1,\"recording_id\":\"" + id + "\",\"duration_ms\":60000,\"size_bytes\":32,"
            "\"started_at_unix_ms\":" + std::to_string(timestamp) + extra + "}");
    }
};
void Finish(WatchRecordingLibrary& library) {
    size_t count = 0;
    while (library.Step()) assert(++count < 1000);
    assert(library.Snapshot().status == WatchLibraryStatus::kReady);
}
void TestMissingAndEmpty() {
    Fixture f;
    WatchRecordingLibrary library;
    assert(!library.Begin((f.path / "missing").string()));
    assert(library.Snapshot().status == WatchLibraryStatus::kUnavailable);
    assert(library.Begin(f.path.string()));
    assert(library.Step(0) && library.Snapshot().entries.empty());
    Finish(library);
    assert(library.Snapshot().entries.empty());
}

void TestIncompleteMetadataRemainsVisible() {
    Fixture f;
    f.Recording("failed", 1, ",\"recording_complete\":false");
    f.Recording("complete", 2, ",\"recording_complete\":true");
    f.Recording("legacy", 3);
    WatchRecordingLibrary library;
    assert(library.Begin(f.path.string())); Finish(library);
    assert(library.Snapshot().entries.size() == 3);
    for (const auto& entry : library.Snapshot().entries) {
        assert(entry.metadata_available && entry.audio_available);
        assert(entry.incomplete == (entry.id == "failed"));
    }
}
void TestRecentBoundedIndex() {
    Fixture f;
    for (unsigned i = 0; i < 100; ++i) f.Recording("record-" + std::to_string(i), 1789290000000ULL + i * 1000);
    WatchRecordingLibrary library;
    assert(library.Begin(f.path.string()));
    assert(library.Step(1000000));
    assert(library.Snapshot().status == WatchLibraryStatus::kLoading && library.Snapshot().entries.empty());
    Finish(library);
    const auto& entries = library.Snapshot().entries;
    assert(entries.size() == WatchRecordingLibrary::kMaxEntries);
    for (unsigned i = 0; i < entries.size(); ++i) {
        assert(entries[i].id == "record-" + std::to_string(99 - i));
        assert(entries[i].metadata_available && entries[i].duration_ms == 60000 && entries[i].size_bytes == 32);
    }
}
void TestInvalidMetadataKeepsAudioVisible() {
    Fixture f;
    const std::vector<std::string> invalid = {
        "{}", "{\"schema_version\":1,\"recording_id\":\"wrong\",\"duration_ms\":60000,\"size_bytes\":32}",
        "{\"schema_version\":1,\"recording_id\":\"bad-2\",\"duration_ms\":-1,\"size_bytes\":32}",
        "{\"schema_version\":1,\"recording_id\":\"bad-3\",\"duration_ms\":0.1,\"size_bytes\":32}",
        "{\"schema_version\":1,\"recording_id\":\"bad-4\",\"duration_ms\":60000,\"size_bytes\":31}",
        std::string(20, '[') + "0" + std::string(20, ']'),
        std::string(WatchRecordingLibrary::kMetadataLimit + 1, ' '),
        std::string("{}\0{}", 5),
        "{\"schema_version\":1,\"recording_id\":\"bad-8\",\"duration_ms\":4294967296,\"size_bytes\":32}",
        "{\"schema_version\":2,\"recording_id\":\"bad-9\",\"duration_ms\":60000,\"size_bytes\":32}"
    };
    for (size_t i = 0; i < invalid.size(); ++i) {
        f.Write("bad-" + std::to_string(i) + ".opus", std::string(32, 'a'));
        f.Write("bad-" + std::to_string(i) + ".opus.json", invalid[i]);
    }
    WatchRecordingLibrary library;
    assert(library.Begin(f.path.string())); Finish(library);
    assert(library.Snapshot().entries.size() == invalid.size());
    for (const auto& entry : library.Snapshot().entries) assert(!entry.metadata_available && entry.duration_ms == 0);
}
void TestExcludedAndUnsafeFiles() {
    Fixture f;
    f.Recording("active", 100);
    f.Recording("saved", 90);
    f.Write("empty.opus", ""); f.Write("invalid.name.opus", "noise");
    f.Write("unrelated.wav", "noise");
    std::filesystem::create_directory(f.path / "directory.opus");
    WatchRecordingLibrary library;
    assert(library.Begin(f.path.string(), "active")); Finish(library);
    assert(library.Snapshot().entries.size() == 1 && library.Snapshot().entries[0].id == "saved");
}
void TestCancelAndRestart() {
    Fixture f;
    f.Recording("one", 100); f.Recording("two", 200);
    WatchRecordingLibrary library;
    assert(library.Begin(f.path.string())); assert(library.Step(1));
    library.Cancel();
    assert(!library.Step() && library.Snapshot().status == WatchLibraryStatus::kInterrupted);
    const auto revision = library.Snapshot().revision;
    for (int i = 0; i < 100; ++i) { assert(library.Begin(f.path.string())); library.Cancel(); }
    assert(library.Begin(f.path.string())); Finish(library);
    assert(library.Snapshot().revision > revision && library.Snapshot().entries.size() == 2);
}
void TestStableSortingAndTimestampBounds() {
    Fixture f;
    f.Recording("a", 100); f.Recording("b", 100);
    f.Recording("rtc", 0, ",\"rtc_epoch_seq\":1,\"started_at_rtc_ms\":1000");
    WatchRecordingLibrary library;
    assert(library.Begin(f.path.string())); Finish(library);
    const auto& entries = library.Snapshot().entries;
    assert(entries[0].id == "rtc" && entries[0].started_at_unix_ms == 0);
    assert(entries[1].id == "b" && entries[2].id == "a");
    assert(entries[0].sort_key == 1000000001000ULL);
    Fixture invalid;
    invalid.Recording("overflow", 0, ",\"rtc_epoch_seq\":18446744073709551615,\"started_at_rtc_ms\":-1");
    assert(library.Begin(invalid.path.string())); Finish(library);
    assert(library.Snapshot().entries[0].started_at_unix_ms == 0 && library.Snapshot().entries[0].metadata_available);
}
void TestBidirectionalPagination() {
    Fixture f;
    for (unsigned i = 0; i < 100; ++i) f.Recording("record-" + std::to_string(i), 1000 + i);
    WatchRecordingLibrary library;
    assert(library.Begin(f.path.string())); Finish(library);
    std::set<std::string> seen;
    for (unsigned page = 0; page < 5; ++page) {
        const auto& snapshot = library.Snapshot();
        assert(snapshot.entries.size() == 20);
        assert(snapshot.has_newer == (page > 0));
        assert(snapshot.has_older == (page < 4));
        for (unsigned i = 0; i < 20; ++i) {
            assert(snapshot.entries[i].id == "record-" + std::to_string(99 - page * 20 - i));
            assert(seen.insert(snapshot.entries[i].id).second);
        }
        if (page < 4) { assert(library.BeginOlder()); Finish(library); }
    }
    assert(seen.size() == 100 && !library.BeginOlder());
    for (int page = 3; page >= 0; --page) {
        assert(library.BeginNewer()); Finish(library);
        const auto& snapshot = library.Snapshot();
        assert(snapshot.entries.size() == 20 && snapshot.has_older);
        assert(snapshot.has_newer == (page > 0));
        for (unsigned i = 0; i < 20; ++i)
            assert(snapshot.entries[i].id == "record-" + std::to_string(99 - page * 20 - i));
    }
    assert(!library.BeginNewer());
}
void TestPageBoundariesAndTies() {
    for (unsigned count : {0U, 1U, 20U, 21U, 41U}) {
        Fixture f;
        std::vector<std::string> expected;
        for (unsigned i = 0; i < count; ++i) {
            auto id = "tied-" + std::to_string(i);
            f.Recording(id, 1000); expected.push_back(id);
        }
        std::sort(expected.begin(), expected.end(), std::greater<std::string>());
        WatchRecordingLibrary library;
        assert(library.Begin(f.path.string())); Finish(library);
        size_t index = 0;
        do {
            for (const auto& entry : library.Snapshot().entries) {
                assert(index < expected.size() && entry.id == expected[index++]);
            }
            if (!library.Snapshot().has_older) break;
            assert(library.BeginOlder()); Finish(library);
        } while (true);
        assert(index == expected.size());
        if (count > 20) {
            assert(library.BeginNewer()); Finish(library);
            assert(library.Snapshot().entries.size() == 20);
            const size_t start = count > 40 ? 20 : 0;
            for (size_t i = 0; i < 20; ++i)
                assert(library.Snapshot().entries[i].id == expected[start + i]);
        }
    }
}
void TestDeletedPageAndRefresh() {
    Fixture f;
    for (unsigned i = 0; i < 21; ++i) f.Recording("record-" + std::to_string(i), 1000 + i);
    WatchRecordingLibrary library;
    assert(library.Begin(f.path.string())); Finish(library);
    std::filesystem::remove(f.path / "record-0.opus");
    std::filesystem::remove(f.path / "record-0.opus.json");
    assert(library.BeginOlder()); Finish(library);
    assert(library.Snapshot().entries.empty() && library.Snapshot().has_newer && !library.Snapshot().has_older);
    assert(library.BeginNewer()); Finish(library);
    assert(library.Snapshot().entries.size() == 20);
    assert(library.Snapshot().entries.back().id == "record-1");
    assert(!library.Snapshot().has_newer && !library.Snapshot().has_older);
    f.Recording("newest", 5000);
    assert(library.Begin(f.path.string())); Finish(library);
    assert(library.Snapshot().entries.front().id == "newest" && library.Snapshot().has_older);
    assert(library.BeginOlder()); assert(library.Step(1)); library.Cancel();
    assert(!library.BeginOlder() && !library.BeginNewer());
    assert(library.Begin(f.path.string())); Finish(library);
    assert(library.Snapshot().entries.front().id == "newest");
}
void TestInsertionDoesNotRepeatOlderPage() {
    Fixture f;
    for (unsigned i = 0; i < 21; ++i) f.Recording("record-" + std::to_string(i), 1000 + i);
    WatchRecordingLibrary library;
    assert(library.Begin(f.path.string())); Finish(library);
    f.Recording("newest", 5000);
    assert(library.BeginOlder()); Finish(library);
    assert(library.Snapshot().entries.size() == 1 && library.Snapshot().entries.front().id == "record-0");
    assert(library.BeginNewer()); Finish(library);
    assert(library.Snapshot().entries.front().id == "record-20" && library.Snapshot().has_newer);
    assert(library.BeginNewer()); Finish(library);
    assert(library.Snapshot().entries.size() == 1 && library.Snapshot().entries.front().id == "newest");
}
void TestConfirmedDeletionInvalidatesCachedEntry() {
    Fixture f; f.Recording("one", 1000); f.Recording("two", 2000);
    WatchRecordingLibrary library;
    assert(library.Begin(f.path.string())); Finish(library);
    const auto revision = library.Snapshot().revision;
    std::filesystem::remove(f.path / "two.opus");
    std::filesystem::remove(f.path / "two.opus.json");
    library.Forget("two");
    assert(library.Snapshot().revision > revision && library.Snapshot().entries.size() == 1);
    assert(library.Snapshot().entries.front().id == "one");
    assert(library.Begin(f.path.string())); Finish(library);
    assert(library.Snapshot().entries.size() == 1 && library.Snapshot().entries.front().id == "one");
}
void TestOrphanMetadataDiscoveryWithoutRuntimeReceipt() {
    Fixture f;
    f.Recording("paired", 1000);
    f.Recording("leftover", 2000);
    f.Write("corrupt.opus.json", "not-json");
    f.Write("empty-meta.opus.json", "");
    {
        WatchRecordingLibrary before;
        assert(before.Begin(f.path.string())); Finish(before);
        std::filesystem::remove(f.path / "leftover.opus");
    }
    WatchRecordingLibrary after;
    assert(after.Begin(f.path.string())); Finish(after);
    assert(after.Snapshot().entries.size() == 4);
    size_t audio = 0, orphans = 0;
    for (const auto& entry : after.Snapshot().entries) {
        if (entry.audio_available) {
            ++audio; assert(entry.id == "paired" && entry.metadata_available);
        } else {
            ++orphans;
            assert(!entry.metadata_available && entry.duration_ms == 0 && entry.started_at_unix_ms == 0);
            assert(entry.file_name == entry.id + ".opus.json");
            assert(entry.size_bytes == std::filesystem::file_size(f.path / entry.file_name));
        }
    }
    assert(audio == 1 && orphans == 3);
    assert(std::filesystem::exists(f.path / "leftover.opus.json"));
    f.Write("leftover.opus", std::string(32, 'a'));
    assert(after.Begin(f.path.string())); Finish(after);
    assert(after.Snapshot().entries.size() == 4);
    for (const auto& entry : after.Snapshot().entries)
        if (entry.id == "leftover") assert(entry.audio_available && entry.metadata_available);
}
void TestOrphanFilteringAndPagination() {
    Fixture f;
    for (unsigned i = 0; i < 45; ++i) f.Write("orphan-" + std::to_string(i) + ".opus.json", "{}");
    f.Write("active.opus.json", "{}");
    f.Write("empty-audio.opus", ""); f.Write("empty-audio.opus.json", "{}");
    f.Write("directory-audio.opus.json", "{}"); std::filesystem::create_directory(f.path / "directory-audio.opus");
    std::filesystem::create_directory(f.path / "directory-meta.opus.json");
    f.Write("unsafe.name.opus.json", "{}"); f.Write("unrelated.json", "{}");
    WatchRecordingLibrary library;
    assert(library.Begin(f.path.string(), "active")); Finish(library);
    std::set<std::string> seen;
    for (;;) {
        for (const auto& entry : library.Snapshot().entries) {
            assert(!entry.audio_available && entry.id.compare(0, 7, "orphan-") == 0);
            assert(seen.insert(entry.id).second);
        }
        if (!library.Snapshot().has_older) break;
        assert(library.BeginOlder()); Finish(library);
    }
    assert(seen.size() == 45);
    assert(library.BeginNewer()); Finish(library);
    assert(library.Snapshot().entries.size() == 20 && library.Snapshot().has_newer);
    assert(library.BeginNewer()); Finish(library);
    assert(library.Snapshot().entries.size() == 20 && !library.Snapshot().has_newer);
}
}
int main() {
    TestMissingAndEmpty(); TestIncompleteMetadataRemainsVisible(); TestRecentBoundedIndex(); TestInvalidMetadataKeepsAudioVisible();
    TestExcludedAndUnsafeFiles(); TestCancelAndRestart(); TestStableSortingAndTimestampBounds();
    TestBidirectionalPagination(); TestPageBoundariesAndTies(); TestDeletedPageAndRefresh();
    TestInsertionDoesNotRepeatOlderPage();
    TestConfirmedDeletionInvalidatesCachedEntry();
    TestOrphanMetadataDiscoveryWithoutRuntimeReceipt(); TestOrphanFilteringAndPagination();
    std::cout << "14 recording catalog tests passed\n";
}
