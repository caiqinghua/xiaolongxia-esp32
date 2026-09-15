#include "watch/watch_motion_task.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {
using namespace std::chrono_literals;

template<class Predicate> void Await(Predicate predicate) {
    const auto until = std::chrono::steady_clock::now() + 3s;
    while (!predicate() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(1ms);
    assert(predicate());
}

struct Fixture {
    std::atomic<bool> pause{false}, fail_stop{false}, fail_save{false}, fail_read{false}, busy_load{false};
    std::atomic<bool> block_save{false}, saving{false};
    std::atomic<unsigned> starts{0}, reads{0}, stops{0}, saves{0};
    std::mutex storage_mutex;
    WatchActivityRecord stored{};
    bool has_record = false;
    std::atomic<bool> corrupt{false};
    std::atomic<bool> fail_load{false};
    uint32_t counter = 0;
    uint64_t generation = 0;
    std::thread::id owner;
    const std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();

    void OnWorker() {
        if (owner == std::thread::id{}) owner = std::this_thread::get_id();
        assert(owner == std::this_thread::get_id());
    }

    WatchMotionTask::Hooks Hooks() {
        return {
            [this]() { OnWorker(); return true; },
            [this]() { OnWorker(); ++starts; counter = 0; return ++generation; },
            [this](WatchImuReading& value) {
                OnWorker(); ++reads;
                if (fail_read) return false;
                value = {generation, ++counter, 0, 0, 1}; return true;
            },
            [this]() { OnWorker(); ++stops; return !fail_stop; },
            [this](WatchActivityRecord& value, bool sleeping) {
                OnWorker();
                if (busy_load && !sleeping) return WatchMotionLoad::kBusy;
                if (fail_load) return WatchMotionLoad::kFailed;
                std::lock_guard<std::mutex> lock(storage_mutex);
                if (corrupt) { value.fill(0xff); return WatchMotionLoad::kLoaded; }
                if (!has_record) return WatchMotionLoad::kAbsent;
                value = stored; return WatchMotionLoad::kLoaded;
            },
            [this](const WatchActivityRecord& value, bool sleeping) {
                OnWorker(); ++saves;
                saving = true;
                while (block_save) std::this_thread::sleep_for(1ms);
                if (sleeping) assert(stops || !starts);
                std::lock_guard<std::mutex> lock(storage_mutex);
                if (fail_save) { saving = false; return false; }
                stored = value; has_record = true; saving = false; return true;
            },
            [this]() {
                OnWorker();
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - epoch).count();
                return WatchMotionClock{static_cast<uint64_t>(ms), 20000};
            },
            [this]() { OnWorker(); return pause.load(); },
        };
    }
};

void TestRecordingAndSleep() {
    Fixture fixture;
    WatchMotionTask task(fixture.Hooks());
    assert(task.Start() && !task.Start());
    Await([&]() { return task.Snapshot().activity.steps > 0; });
    fixture.pause = true;
    Await([&]() { return fixture.stops > 0 && task.Snapshot().activity.status == WatchMotionStatus::kPaused; });
    const auto reads = fixture.reads.load();
    std::this_thread::sleep_for(300ms);
    assert(fixture.reads == reads);
    fixture.pause = false;
    Await([&]() { return fixture.starts >= 2; });
    assert(task.Suspend(2000));
    assert(task.Snapshot().initialized && !task.Snapshot().storage_error);
    const auto parked_reads = fixture.reads.load();
    const auto parked_starts = fixture.starts.load();
    const auto parked_saves = fixture.saves.load();
    std::this_thread::sleep_for(400ms);
    assert(fixture.reads == parked_reads && fixture.starts == parked_starts && fixture.saves == parked_saves);
    assert(task.Resume(2000));
    Await([&]() { return fixture.starts > parked_starts; });
    assert(task.Suspend(2000));
}

void TestSleepFailureDoesNotReleasePeripherals() {
    Fixture fixture;
    WatchMotionTask task(fixture.Hooks());
    assert(task.Start());
    Await([&]() { return fixture.starts > 0; });
    fixture.fail_stop = true;
    assert(!task.Suspend(30));
    Await([&]() { return task.Snapshot().activity.status == WatchMotionStatus::kFault; });
    assert(fixture.saves == 0);
    fixture.fail_stop = false;
    fixture.fail_save = true;
    assert(!task.Suspend(1100));
    assert(task.Snapshot().storage_error);
    fixture.fail_save = false;
    assert(task.Suspend(2000));
    assert(!task.Snapshot().storage_error);
}

void TestCancelledSleepWaitsForWrite() {
    Fixture fixture;
    WatchMotionTask task(fixture.Hooks());
    assert(task.Start());
    Await([&]() { return fixture.starts > 0; });
    fixture.block_save = true;
    assert(!task.Suspend(10));
    Await([&]() { return fixture.saving.load(); });
    const auto started = std::chrono::steady_clock::now();
    assert(task.Snapshot().initialized);
    assert(std::chrono::steady_clock::now() - started < 100ms);
    assert(!task.Resume(10));
    assert(fixture.starts == 1);
    fixture.block_save = false;
    assert(task.Resume(2000));
    Await([&]() { return fixture.starts == 2; });
    assert(task.Suspend(2000));
}

void TestPersistenceAndCorruption() {
    Fixture fixture;
    {
        WatchMotionTask task(fixture.Hooks());
        assert(task.Start());
        Await([&]() { return task.Snapshot().activity.steps >= 2; });
        assert(task.Suspend(2000));
    }
    WatchActivityCheckpoint saved;
    assert(DecodeWatchActivityCheckpoint(fixture.stored.data(), fixture.stored.size(), saved));
    assert(saved.count == 1 && saved.days[0].steps >= 2);
    fixture.owner = {};
    fixture.pause = true;
    {
        WatchMotionTask task(fixture.Hooks());
        assert(task.Start());
        Await([&]() { return task.Snapshot().initialized; });
        assert(task.Snapshot().activity.steps == saved.days[0].steps);
        assert(task.Suspend(2000));
    }
    fixture.owner = {};
    fixture.corrupt = true;
    const auto writes = fixture.saves.load();
    const auto starts = fixture.starts.load();
    {
        WatchMotionTask task(fixture.Hooks());
        assert(task.Start());
        Await([&]() { return task.Snapshot().storage_error; });
        assert(!task.Snapshot().initialized);
        assert(task.Suspend(2000));
        assert(fixture.saves == writes && fixture.starts == starts);
    }
}

void TestBusyLoadAndReadFault() {
    Fixture fixture;
    fixture.busy_load = true;
    WatchMotionTask task(fixture.Hooks());
    assert(task.Start());
    std::this_thread::sleep_for(30ms);
    assert(!task.Snapshot().initialized && !task.Snapshot().storage_error && fixture.starts == 0);
    assert(task.Suspend(2000));
    assert(task.Snapshot().initialized);
    fixture.busy_load = false;
    assert(task.Resume(2000));
    Await([&]() { return task.Snapshot().activity.steps > 0; });
    fixture.fail_read = true;
    Await([&]() { return task.Snapshot().activity.status == WatchMotionStatus::kFault; });
    const auto steps = task.Snapshot().activity.steps;
    std::this_thread::sleep_for(300ms);
    assert(task.Snapshot().activity.steps == steps);
    fixture.fail_read = false;
    Await([&]() { return task.Snapshot().activity.status == WatchMotionStatus::kRunning; });
    assert(task.Suspend(2000));
}

void TestDateChangesWhileStarting() {
    Fixture fixture;
    auto hooks = fixture.Hooks();
    std::optional<int32_t> day = 20000;
    const auto start = hooks.start;
    hooks.start = [&]() {
        const auto generation = start();
        day = 20001;
        return generation;
    };
    const auto clock = hooks.clock;
    hooks.clock = [&]() {
        auto captured = clock();
        captured.day = day;
        return captured;
    };
    WatchMotionTask task(std::move(hooks));
    assert(task.Start());
    Await([&]() { return task.Snapshot().activity.steps >= 1; });
    assert(task.Suspend(2000));
    const auto snapshot = task.Snapshot();
    assert(snapshot.activity.day == 20001);
    assert(snapshot.activity.undated_steps == 0);
    WatchActivityCheckpoint saved;
    assert(DecodeWatchActivityCheckpoint(fixture.stored.data(), fixture.stored.size(), saved));
    assert(saved.count == 1 && saved.days[0].day == 20001);
}

void TestStorageRetryPreservesUncommittedSteps() {
    Fixture fixture;
    WatchMotionTask task(fixture.Hooks());
    assert(!task.RetryStorage(1));
    assert(task.Start());
    Await([&]() { return task.Snapshot().activity.steps >= 2; });
    fixture.fail_save = true;
    fixture.pause = true;
    Await([&]() { return task.Snapshot().storage_error; });
    const auto steps = task.Snapshot().activity.steps;
    assert(task.Snapshot().history.count == 1 && task.Snapshot().history.days[0].steps == steps);
    fixture.block_save = true;
    assert(!task.RetryStorage(0));
    assert(task.RetryStorage(1));
    Await([&]() { return fixture.saving.load(); });
    assert(task.Snapshot().retry_pending && !task.Snapshot().retry_succeeded);
    assert(!task.RetryStorage(2));
    fixture.fail_save = false;
    fixture.block_save = false;
    Await([&]() { return !task.Snapshot().retry_pending; });
    assert(task.Snapshot().retry_succeeded && !task.Snapshot().storage_error);
    assert(task.Snapshot().activity.steps == steps);
    assert(!task.RetryStorage(1));
    assert(task.Suspend(2000));
    assert(!task.RetryStorage(2));
    WatchActivityCheckpoint saved;
    assert(DecodeWatchActivityCheckpoint(fixture.stored.data(), fixture.stored.size(), saved));
    assert(saved.count == 1 && saved.days[0].steps == steps);
}

void TestStorageReloadRetryDoesNotEraseCorruptHistory() {
    Fixture fixture;
    fixture.corrupt = true;
    fixture.pause = true;
    WatchMotionTask task(fixture.Hooks());
    assert(task.Start());
    Await([&]() { return task.Snapshot().storage_error; });
    assert(task.RetryStorage(1));
    Await([&]() { return !task.Snapshot().retry_pending; });
    assert(!task.Snapshot().retry_succeeded && fixture.saves == 0 && fixture.starts == 0);
    WatchActivityCheckpoint retained;
    retained.count = 1;
    retained.days[0] = {20000, 1234};
    {
        std::lock_guard<std::mutex> lock(fixture.storage_mutex);
        fixture.stored = *EncodeWatchActivityCheckpoint(retained);
        fixture.has_record = true;
    }
    fixture.corrupt = false;
    assert(task.RetryStorage(2));
    Await([&]() { return !task.Snapshot().retry_pending; });
    const auto result = task.Snapshot();
    assert(result.retry_succeeded && result.initialized && !result.storage_error);
    assert(result.activity.steps == 1234 && result.history.days[0].steps == 1234);
    assert(task.Suspend(2000));
}

void TestSleepSupersedesQueuedStorageRetry() {
    Fixture fixture;
    std::atomic<bool> blocked{true}, reading{false};
    auto hooks = fixture.Hooks();
    const auto read = hooks.read;
    hooks.read = [&](WatchImuReading& value) {
        reading = true;
        while (blocked) std::this_thread::sleep_for(1ms);
        return read(value);
    };
    WatchMotionTask task(std::move(hooks));
    assert(task.Start());
    Await([&]() { return reading.load(); });
    assert(task.RetryStorage(1));
    assert(!task.Suspend(10));
    assert(task.Snapshot().retry_pending);
    blocked = false;
    assert(task.Suspend(2000));
    const auto result = task.Snapshot();
    assert(!result.retry_pending && !result.retry_succeeded);
    assert(result.retry_request == 1);
}

void TestConfirmedCorruptReset() {
    Fixture fixture;
    fixture.corrupt = true;
    auto hooks = fixture.Hooks();
    const auto read = hooks.read;
    hooks.read = [read](WatchImuReading& value) { const bool ok = read(value); value.steps = 0; return ok; };
    WatchMotionTask task(std::move(hooks));
    assert(task.Start());
    Await([&]() { return task.Snapshot().storage_error; });
    const auto token = task.Snapshot().repair_token;
    assert(token && !task.ResetCorruptStorage(1, 0));
    assert(task.RetryStorage(1));
    Await([&]() { return !task.Snapshot().retry_pending; });
    assert(!task.ResetCorruptStorage(2, token));
    const auto current = task.Snapshot().repair_token;
    assert(current && current != token);
    fixture.block_save = true;
    assert(task.ResetCorruptStorage(2, current));
    Await([&]() { return fixture.saving.load(); });
    assert(task.Snapshot().reset_pending && !task.Snapshot().reset_succeeded);
    assert(!task.RetryStorage(3) && !task.ResetCorruptStorage(3, current));
    assert(!task.Suspend(10));
    fixture.block_save = false;
    assert(task.Suspend(2000));
    const auto result = task.Snapshot();
    assert(!result.reset_pending && result.reset_succeeded && result.initialized && !result.storage_error);
    assert(result.repair_token == 0 && result.history.count == 0 && result.history.undated_steps == 0);
    assert(result.history.incomplete && fixture.has_record);
    WatchActivityCheckpoint stored;
    assert(DecodeWatchActivityCheckpoint(fixture.stored.data(), fixture.stored.size(), stored));
    assert(stored.count == 0 && stored.undated_steps == 0);
    assert(!task.ResetCorruptStorage(3, current));
}

void TestResetPreservesRecoveredRecordAndReadErrors() {
    Fixture fixture;
    fixture.fail_load = true;
    WatchMotionTask task(fixture.Hooks());
    assert(task.Start());
    Await([&]() { return task.Snapshot().storage_error; });
    assert(task.Snapshot().repair_token == 0 && !task.ResetCorruptStorage(1, 1));
    fixture.corrupt = true;
    fixture.fail_load = false;
    assert(task.RetryStorage(1));
    Await([&]() { return !task.Snapshot().retry_pending; });
    auto token = task.Snapshot().repair_token;
    fixture.fail_load = true;
    assert(task.ResetCorruptStorage(2, token));
    Await([&]() { return !task.Snapshot().reset_pending; });
    assert(!task.Snapshot().reset_succeeded && fixture.saves == 0 && !task.Snapshot().repair_token);
    fixture.fail_load = false;
    assert(task.RetryStorage(3));
    Await([&]() { return !task.Snapshot().retry_pending; });
    token = task.Snapshot().repair_token;
    WatchActivityCheckpoint recovered;
    recovered.count = 1;
    recovered.days[0] = {20000, 777};
    {
        std::lock_guard<std::mutex> lock(fixture.storage_mutex);
        fixture.stored = *EncodeWatchActivityCheckpoint(recovered);
        fixture.has_record = true;
    }
    fixture.corrupt = false;
    assert(task.ResetCorruptStorage(4, token));
    Await([&]() { return !task.Snapshot().reset_pending; });
    assert(task.Snapshot().reset_succeeded && task.Snapshot().activity.steps >= 777);
    assert(fixture.saves == 0);
    assert(task.Suspend(2000));
}

void TestResetCommitAmbiguityRequiresFreshRead() {
    Fixture fixture;
    fixture.corrupt = true;
    auto hooks = fixture.Hooks();
    const auto save = hooks.save;
    hooks.save = [&](const WatchActivityRecord& value, bool sleeping) {
        assert(save(value, sleeping));
        fixture.corrupt = false;
        return false;
    };
    WatchMotionTask task(std::move(hooks));
    assert(task.Start());
    Await([&]() { return task.Snapshot().storage_error; });
    assert(task.ResetCorruptStorage(1, task.Snapshot().repair_token));
    Await([&]() { return !task.Snapshot().reset_pending; });
    assert(!task.Snapshot().reset_succeeded && !task.Snapshot().initialized);
    assert(task.Snapshot().storage_error && !task.Snapshot().repair_token && fixture.saves == 1);
    fixture.pause = true;
    assert(task.RetryStorage(2));
    Await([&]() { return !task.Snapshot().retry_pending; });
    assert(task.Snapshot().retry_succeeded && task.Snapshot().initialized && fixture.saves == 1);
    assert(task.Suspend(2000));
}

void TestResetRecheckCancelledForSleep() {
    Fixture fixture;
    fixture.corrupt = true;
    std::atomic<bool> block_load{false}, loading{false};
    auto hooks = fixture.Hooks();
    const auto load = hooks.load;
    hooks.load = [&](WatchActivityRecord& value, bool sleeping) {
        loading = true;
        while (block_load) std::this_thread::sleep_for(1ms);
        return load(value, sleeping);
    };
    WatchMotionTask task(std::move(hooks));
    assert(task.Start());
    Await([&]() { return task.Snapshot().storage_error; });
    block_load = true;
    loading = false;
    assert(task.ResetCorruptStorage(1, task.Snapshot().repair_token));
    Await([&]() { return loading.load(); });
    assert(!task.Suspend(10));
    block_load = false;
    assert(task.Suspend(2000));
    assert(!task.Snapshot().reset_pending && !task.Snapshot().reset_succeeded && fixture.saves == 0);
}

void TestResetDeferredDuringRecording() {
    Fixture fixture;
    fixture.corrupt = true;
    fixture.pause = true;
    WatchMotionTask task(fixture.Hooks());
    assert(task.Start());
    Await([&]() { return task.Snapshot().storage_error; });
    auto token = task.Snapshot().repair_token;
    assert(task.ResetCorruptStorage(1, token));
    Await([&]() { return !task.Snapshot().reset_pending; });
    assert(!task.Snapshot().reset_succeeded && fixture.saves == 0 && fixture.starts == 0);
    fixture.pause = false;
    assert(task.ResetCorruptStorage(2, task.Snapshot().repair_token));
    Await([&]() { return !task.Snapshot().reset_pending; });
    assert(task.Snapshot().reset_succeeded && fixture.saves >= 1);
    assert(task.Suspend(2000));
}
void TestMotionObservationHonorsReadAndRecordingBoundaries() {
    Fixture fixture;
    std::atomic<unsigned> samples{0}, read_failures{0}, denied{0};
    std::atomic<bool> pause_during_read{false};
    auto hooks = fixture.Hooks();
    const auto read = hooks.read;
    hooks.read = [&](WatchImuReading& reading) {
        const bool result = read(reading);
        if (pause_during_read) fixture.pause = true;
        return result;
    };
    hooks.motion_observation = [&](const WatchImuReading* reading, uint64_t, bool allowed) {
        fixture.OnWorker();
        if (reading) {
            assert(allowed && !fixture.pause && reading->generation == fixture.generation);
            assert(reading->z_g == 1);
            ++samples;
        } else if (allowed) ++read_failures;
        else ++denied;
    };
    WatchMotionTask task(std::move(hooks));
    assert(task.Start());
    Await([&]() { return samples > 0; });
    const auto before_failure = read_failures.load();
    fixture.fail_read = true;
    Await([&]() { return read_failures > before_failure; });
    const auto before_recovery = samples.load();
    fixture.fail_read = false;
    Await([&]() { return samples > before_recovery; });
    pause_during_read = true;
    Await([&]() { return denied > 0 && fixture.stops > 0; });
    const auto paused_samples = samples.load();
    std::this_thread::sleep_for(300ms);
    assert(samples == paused_samples);
    assert(task.Suspend(2000));
}
}  // namespace

int main() {
    TestRecordingAndSleep();
    TestSleepFailureDoesNotReleasePeripherals();
    TestCancelledSleepWaitsForWrite();
    TestPersistenceAndCorruption();
    TestBusyLoadAndReadFault();
    TestDateChangesWhileStarting();
    TestStorageRetryPreservesUncommittedSteps();
    TestStorageReloadRetryDoesNotEraseCorruptHistory();
    TestSleepSupersedesQueuedStorageRetry();
    TestConfirmedCorruptReset();
    TestResetPreservesRecoveredRecordAndReadErrors();
    TestResetCommitAmbiguityRequiresFreshRead();
    TestResetRecheckCancelledForSleep();
    TestResetDeferredDuringRecording();
    TestMotionObservationHonorsReadAndRecordingBoundaries();
    std::puts("Watch motion worker: 15 test groups passed");
}
