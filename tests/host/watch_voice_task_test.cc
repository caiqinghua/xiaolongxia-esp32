#include "watch/watch_voice_task.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace watch_voice;
using namespace std::chrono_literals;

namespace {
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool ready = false;
    std::atomic<bool> retired{false};
    void Wait() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [this]() { return ready; });
    }
    void AwaitEntry() {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, 2s, [this]() { return entered; }));
    }
    void Release() {
        std::lock_guard<std::mutex> lock(mutex);
        ready = true;
        changed.notify_all();
    }
};

bool PollUntilDone(RequestTask& task, Result& status, Response& result) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (task.Poll(status, result)) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

void TestDelayedCleanupAndCancellation() {
    RequestTask task;
    Gate gate;
    std::atomic<int> leases{0};
    assert(task.Start([&](const Client::Cancelled& cancelled, Response& result) {
        gate.Wait();
        assert(cancelled());
        std::get<Bootstrap>(result).access_token = "late-token";
        gate.retired = true;
        return Result::kOk;
    }, [&]() { ++leases; return true; }, [&]() {
        assert(gate.retired);
        --leases;
    }));
    gate.AwaitEntry();
    assert(task.Active() && leases == 1);
    assert(!task.Start([](const auto&, auto&) { return Result::kOk; }, []() { return true; }, []() {}));
    assert(!task.StopWithin(0));
    assert(task.Active() && leases == 1);
    Result status;
    Response result;
    assert(!task.Poll(status, result));
    gate.Release();
    assert(PollUntilDone(task, status, result));
    assert(status == Result::kCancelled && std::get<Bootstrap>(result).access_token.empty());
    assert(!task.Active() && leases == 0);
    assert(!task.Poll(status, result));
    std::puts("PASS: cancellation retains lease until cleanup and join; late output is discarded");
}

void TestReentrantPollDuringHandoff() {
    RequestTask task;
    Gate gate;
    std::atomic<int> leases{0};
    assert(task.Start([&](const auto&, auto&) { gate.Wait(); return Result::kOk; },
                      [&]() { ++leases; return true; }, [&]() { --leases; }));
    gate.AwaitEntry();
    std::atomic<bool> stopping{false};
    std::thread owner([&]() {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        stopping = true;
        bool stopped = false;
        while (!stopped && std::chrono::steady_clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) break;
            // Poll can acquire the owner mutex between the flag and StopWithin.
            // Lock contention is a retry, not proof of a failed worker teardown.
            stopped = task.StopWithin(static_cast<uint32_t>(remaining));
            if (!stopped) std::this_thread::yield();
        }
        assert(stopped);
    });
    while (!stopping) std::this_thread::yield();
    // The main-loop poll never waits behind the handoff's cleanup deadline.
    const auto start = std::chrono::steady_clock::now();
    Result status;
    Response result;
    for (int i = 0; i < 100; ++i) {
        assert(task.Active());
        assert(!task.Poll(status, result));
    }
    assert(std::chrono::steady_clock::now() - start < 500ms);
    assert(leases == 1);
    gate.Release();
    owner.join();
    assert(leases == 0 && !task.Active());
}

void TestReuseAndReservations() {
    RequestTask task;
    int acquired = 0, released = 0;
    const auto work = [](const auto&, Response& result) {
        std::get<Bootstrap>(result).access_token = "current-token";
        return Result::kOk;
    };
    assert(!task.Start(work, []() { return false; }, [&]() { ++released; }));
    assert(!task.Active() && released == 0);
    for (int iteration = 0; iteration < 5; ++iteration) {
        assert(task.Start(work, [&]() { ++acquired; return true; }, [&]() { ++released; }));
        Result status;
        Response result;
        assert(PollUntilDone(task, status, result));
        assert(status == Result::kOk && std::get<Bootstrap>(result).access_token == "current-token");
        assert(acquired == released);
        assert(task.StopWithin(0));
    }
    std::puts("PASS: failed reservation and repeated completed requests balance ownership");
}

void TestCancelAfterWorkBeforePoll() {
    RequestTask task;
    std::atomic<bool> returned{false};
    int released = 0;
    assert(task.Start([&](const auto&, Response& result) {
        std::get<Bootstrap>(result).access_token = "stale-token";
        returned = true;
        return Result::kOk;
    }, []() { return true; }, [&]() { ++released; }));
    while (!returned) std::this_thread::yield();
    task.Cancel();
    Result status;
    Response result;
    assert(PollUntilDone(task, status, result));
    assert(status == Result::kCancelled && std::get<Bootstrap>(result).access_token.empty() && released == 1);
    std::puts("PASS: local intent after work completion still invalidates the unconsumed result");
}
}

int main() {
    TestDelayedCleanupAndCancellation();
    for (int iteration = 0; iteration < 32; ++iteration) TestReentrantPollDuringHandoff();
    std::puts("PASS: main-loop polling stays nonblocking during concurrent handoff (32 runs)");
    TestReuseAndReservations();
    TestCancelAfterWorkBeforePoll();
}
