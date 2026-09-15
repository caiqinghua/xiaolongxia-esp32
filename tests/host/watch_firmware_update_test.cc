#include "watch/watch_firmware_update.h"
#include "firmware_release_fixture.h"
#include "audio/audio_session_coordinator.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using watch_update::Controller;
using watch_update::Operation;
using watch_update::Phase;
using firmware_release::CheckStatus;

template<class Predicate> void Wait(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

struct Fixture {
    std::atomic<int64_t> now{1000};
    std::atomic<bool> blocked{false}, proceed{false};
    int block_stage = 0, acquired = 0, released = 0;
    bool acquire_ok = true, release_ok = true, install_ok = true, io_released = true;
    bool release_saw_io_failure = false;
    bool omit_verified = false;
    CheckStatus check_status = CheckStatus::kAvailable;
    std::string envelope = release_fixture::Seal(release_fixture::Payload(std::string(5000, 'x')));
    firmware_release::CheckResult checked;
    Fixture() {
        checked.status = CheckStatus::kAvailable;
        checked.envelope = envelope;
        checked.release = firmware_release::Verify(envelope, "test-watch", 7, release_fixture::PublicKey());
        assert(checked.release);
    }
    void Barrier(int stage) {
        if (stage != block_stage) return;
        blocked = true;
        Wait([&] { return proceed.load(); });
    }
    Controller::Hooks Hooks() {
        return {
            [&](const Controller::Cancelled&) {
                Barrier(1);
                auto result = checked;
                result.status = check_status;
                if (omit_verified) result.release.reset();
                return result;
            },
            [&](const std::string& approved, const Controller::Progress& progress,
                const Controller::Cancelled& cancelled, const Controller::Cancelled& commit) {
                assert(approved == envelope);
                progress(80, 1000);
                progress(10, 500);
                progress(100, 700);
                Barrier(2);
                if (cancelled() || !commit()) return firmware_release::InstallResult{false, io_released};
                Barrier(3);
                return firmware_release::InstallResult{install_ok, io_released};
            },
            [&](Operation) { if (acquire_ok) ++acquired; return acquire_ok; },
            [&](Operation, bool io_safe) { ++released; release_saw_io_failure = !io_safe; return release_ok && io_safe; },
            [&] { return now.load(); },
            [] { return true; }
        };
    }
};

void Finish(Controller& controller) {
    Wait([&] { controller.Poll(); return !controller.Read().active; });
}
void Available(Controller& controller) {
    assert(controller.Check());
    Finish(controller);
    assert(controller.Read().phase == Phase::kAvailable);
}

void TestCoordinatorHandoff() {
    for (int stage : {1, 2, 3}) {
        for (bool io_safe : {true, false}) {
            Fixture f;
            AudioSessionCoordinator coordinator;
            uint64_t generation = 0;
            auto hooks = f.Hooks();
            hooks.acquire = [&](Operation) {
                const auto lease = coordinator.Begin(AudioSessionMode::kFirmwareUpdate);
                generation = lease.generation;
                ++f.acquired;
                return coordinator.Complete(lease);
            };
            hooks.pin_commit = [&] { return coordinator.BeginFirmwareCommit(generation); };
            hooks.release = [&](Operation, bool safe) {
                ++f.released;
                if (!coordinator.IsCurrent(generation)) {
                    assert(!coordinator.ReleaseFirmwareUpdate(generation, safe));
                    return safe;
                }
                return coordinator.ReleaseFirmwareUpdate(generation, safe);
            };
            Controller controller(std::move(hooks));
            if (stage != 1) Available(controller);
            f.block_stage = stage;
            f.io_released = io_safe;
            assert(stage == 1 ? controller.Check() : controller.Confirm(controller.Read().attempt, 8));
            Wait([&] { return f.blocked.load(); });
            const auto recording = coordinator.Begin(AudioSessionMode::kLocalRecording);
            assert(static_cast<bool>(recording) == (stage != 3));
            assert(!controller.StopWithin(0));
            assert(f.released == (stage == 1 ? 0 : 1));
            f.proceed = true;
            Finish(controller);
            if (stage == 3) {
                assert(coordinator.Mode() == AudioSessionMode::kFirmwareCommit);
                assert(!controller.StopWithin(0));
                assert(controller.Read().boot_selected);
                assert(!coordinator.RecoverToIdle(true));
            } else {
                const bool safe = stage == 1 || io_safe;
                assert(controller.StopWithin(0) == safe);
                assert(coordinator.IsCurrent(recording.generation));
                if (safe) assert(coordinator.Complete(recording));
                else assert(coordinator.Fail(recording, false));
                assert(coordinator.Mode() == (safe ? AudioSessionMode::kLocalRecording : AudioSessionMode::kFault));
                assert(!controller.Read().commit_started && !controller.Read().boot_selected);
            }
        }
    }
}

int main() {
    assert(sodium_init() >= 0);
    TestCoordinatorHandoff();
    {
        Fixture f;
        auto hooks = f.Hooks();
        hooks.pin_commit = {};
        Controller controller(std::move(hooks));
        Available(controller);
        assert(!controller.Confirm(controller.Read().attempt, 8) && f.acquired == 1);
    }
    {
        Fixture f;
        auto hooks = f.Hooks();
        hooks.pin_commit = [] { return false; };
        Controller controller(std::move(hooks));
        Available(controller);
        assert(controller.Confirm(controller.Read().attempt, 8));
        Finish(controller);
        assert(controller.Read().phase == Phase::kInstallFailed && !controller.Read().commit_started);
        assert(controller.StopWithin(0));
    }
    {
        Fixture f;
        Controller controller(f.Hooks());
        Available(controller);
        auto snapshot = controller.Read();
        assert(snapshot.version == "3.0.0" && snapshot.bytes == 5000 && snapshot.sequence == 8);
        assert(!controller.Confirm(snapshot.attempt + 1, 8) && !controller.Confirm(snapshot.attempt, 9));
        assert(controller.Confirm(snapshot.attempt, 8));
        Finish(controller);
        snapshot = controller.Read();
        assert(snapshot.phase == Phase::kRebootRequired && snapshot.boot_selected && snapshot.progress == 100);
        assert(!controller.Cancel(snapshot.attempt) && !controller.Check());
        assert(f.acquired == 2 && f.released == 2);
    }
    for (int stage : {1, 2}) {
        Fixture f;
        Controller controller(f.Hooks());
        if (stage == 2) Available(controller);
        f.block_stage = stage;
        assert(stage == 1 ? controller.Check() : controller.Confirm(controller.Read().attempt, 8));
        Wait([&] { return f.blocked.load(); });
        auto snapshot = controller.Read();
        assert(!controller.Check() && !controller.Cancel(snapshot.attempt + 1));
        assert(!controller.StopWithin(0));
        assert(controller.Read().active && f.released == stage - 1);
        if (stage == 2) assert(snapshot.progress == 99 && !snapshot.boot_selected);
        f.proceed = true;
        Finish(controller);
        assert(controller.Read().phase == Phase::kCancelled && f.acquired == f.released);
    }
    for (bool installed : {true, false}) {
        Fixture f;
        Controller controller(f.Hooks());
        Available(controller);
        f.block_stage = 3;
        f.install_ok = installed;
        assert(controller.Confirm(controller.Read().attempt, 8));
        Wait([&] { return f.blocked.load(); });
        assert(controller.Read().phase == Phase::kCommitting);
        assert(!controller.Cancel(controller.Read().attempt) && !controller.StopWithin(0));
        assert(controller.Read().active && f.released == 1);
        f.proceed = true;
        Finish(controller);
        assert(controller.Read().phase == (installed ? Phase::kRebootRequired : Phase::kCommitUncertain));
        assert(!controller.Check() && f.released == 2);
    }
    for (int64_t time : {999, 121000}) {
        Fixture f;
        Controller controller(f.Hooks());
        Available(controller);
        f.now = time;
        assert(!controller.Confirm(controller.Read().attempt, 8));
        assert(controller.Read().phase == Phase::kExpired && f.acquired == 1);
        f.now = 200000;
        Available(controller);
        assert(controller.Read().attempt == 2);
    }
    for (auto status : {CheckStatus::kNoUpdate, CheckStatus::kNotConfigured, CheckStatus::kTransportError,
                        CheckStatus::kInvalidRelease, CheckStatus::kCancelled}) {
        Fixture f;
        f.check_status = status;
        Controller controller(f.Hooks());
        assert(controller.Check());
        Finish(controller);
        assert(controller.Read().check_status == status);
        assert(!controller.Confirm(controller.Read().attempt, 8));
        assert(controller.Read().version.empty() && f.acquired == f.released);
    }
    {
        Fixture f;
        f.omit_verified = true;
        Controller controller(f.Hooks());
        assert(controller.Check());
        Finish(controller);
        assert(controller.Read().phase == Phase::kCheckFailed);
    }
    {
        Fixture f;
        f.acquire_ok = false;
        Controller controller(f.Hooks());
        assert(!controller.Check() && f.released == 0);
        f.acquire_ok = true;
        f.release_ok = false;
        assert(controller.Check());
        Finish(controller);
        assert(controller.Read().phase == Phase::kResourceFault && !controller.Check());
    }
    {
        Fixture f;
        auto hooks = f.Hooks();
        Controller* pointer = nullptr;
        hooks.acquire = [&](Operation) {
            ++f.acquired;
            assert(pointer->Cancel(pointer->Read().attempt));
            return true;
        };
        Controller controller(std::move(hooks));
        pointer = &controller;
        assert(!controller.Check() && controller.Read().phase == Phase::kCancelled);
        assert(f.acquired == 1 && f.released == 1);
    }
    {
        Fixture f;
        Controller controller(f.Hooks());
        Available(controller);
        f.release_ok = false;
        assert(controller.Confirm(controller.Read().attempt, 8));
        Finish(controller);
        assert(controller.Read().phase == Phase::kResourceFault && controller.Read().boot_selected);
        assert(!controller.Check() && !controller.Cancel(controller.Read().attempt));
    }
    {
        Fixture f;
        Controller controller(f.Hooks());
        Available(controller);
        f.io_released = false;
        f.block_stage = 2;
        assert(controller.Confirm(controller.Read().attempt, 8));
        Wait([&] { return f.blocked.load(); });
        assert(controller.Cancel(controller.Read().attempt));
        f.proceed = true;
        Finish(controller);
        assert(controller.Read().phase == Phase::kResourceFault && f.release_saw_io_failure);
        assert(!controller.Read().boot_selected && !controller.Check());
    }
    std::cout << "PASS: firmware update owner, confirmation, cancellation, commit boundary and resource lifetime\n";
}
