#include "protocols/assistant_transport_handoff.h"
#include "audio/audio_session_coordinator.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

struct OwnerQueue {
    const std::thread::id owner = std::this_thread::get_id();
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::function<void()>> tasks;
    void Push(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mutex);
        tasks.push_back(std::move(task));
        ready.notify_all();
    }
    std::function<void()> Take() {
        std::unique_lock<std::mutex> lock(mutex);
        assert(ready.wait_for(lock, 2s, [&]() { return !tasks.empty(); }));
        auto result = std::move(tasks.front());
        tasks.pop_front();
        return result;
    }
};

static AssistantTransportHandoff MakeHandoff(OwnerQueue& queue, AudioSessionCoordinator& coordinator,
    AssistantTransportHandoff::Close close) {
    return AssistantTransportHandoff(
        [&](std::function<void()> task) { queue.Push(std::move(task)); },
        [&]() { return std::this_thread::get_id() == queue.owner; },
        [&](uint64_t generation) {
            return coordinator.HoldsExclusiveResources(generation);
        }, std::move(close));
}

static void TestOwnerAndWorkerRelease() {
    OwnerQueue queue;
    AudioSessionCoordinator coordinator;
    const auto transition = coordinator.Begin(AudioSessionMode::kLocalRecording);
    unsigned calls = 0;
    bool available = false;
    auto handoff = MakeHandoff(queue, coordinator, [&](uint32_t timeout) {
        assert(std::this_thread::get_id() == queue.owner);
        assert(timeout <= 1000);
        ++calls;
        return available;
    });
    assert(!handoff.CloseWithin(transition.generation, 0));
    available = true;
    assert(handoff.CloseWithin(transition.generation, 0));
    auto worker = std::async(std::launch::async, [&]() { return handoff.CloseWithin(transition.generation, 1000); });
    queue.Take()();
    assert(worker.get() && calls == 3);
    assert(queue.tasks.empty());
}

static void TestExpiredRequestCannotCloseLaterSession() {
    OwnerQueue queue;
    AudioSessionCoordinator coordinator;
    const auto transition = coordinator.Begin(AudioSessionMode::kLocalRecording);
    unsigned calls = 0;
    auto handoff = MakeHandoff(queue, coordinator, [&](uint32_t) { ++calls; return true; });
    auto worker = std::async(std::launch::async, [&]() { return handoff.CloseWithin(transition.generation, 20); });
    auto delayed = queue.Take();
    assert(!worker.get());
    assert(coordinator.Fail(transition, true));
    assert(coordinator.Begin(AudioSessionMode::kAssistantConnecting));
    delayed();
    assert(calls == 0);
}

static void TestStaleGenerationAndWorkerPoll() {
    OwnerQueue queue;
    AudioSessionCoordinator coordinator;
    const auto transition = coordinator.Begin(AudioSessionMode::kSleeping);
    unsigned calls = 0;
    auto handoff = MakeHandoff(queue, coordinator, [&](uint32_t) { ++calls; return true; });
    assert(!handoff.CloseWithin(transition.generation + 1, 1000));
    auto poll = std::async(std::launch::async, [&]() { return handoff.CloseWithin(transition.generation, 0); });
    assert(!poll.get() && queue.tasks.empty());
    auto worker = std::async(std::launch::async, [&]() { return handoff.CloseWithin(transition.generation, 1000); });
    auto delayed = queue.Take();
    assert(coordinator.Fail(transition, true));
    assert(coordinator.Begin(AudioSessionMode::kLocalRecording));
    delayed();
    assert(!worker.get() && calls == 0);
}

static void TestTimeoutDuringOwnerCloseStaysFailed() {
    OwnerQueue queue;
    AudioSessionCoordinator coordinator;
    const auto transition = coordinator.Begin(AudioSessionMode::kLocalRecording);
    std::promise<void> entered, release;
    auto released = release.get_future();
    auto handoff = MakeHandoff(queue, coordinator, [&](uint32_t) {
        entered.set_value();
        released.wait();
        return true;
    });
    auto worker = std::async(std::launch::async, [&]() { return handoff.CloseWithin(transition.generation, 100); });
    auto task = queue.Take();
    auto observer = std::async(std::launch::async, [&]() {
        entered.get_future().wait();
        assert(!worker.get());
        release.set_value();
    });
    task();
    observer.get();
    assert(coordinator.IsCurrent(transition.generation) && coordinator.BlocksSleep());
}

static void TestFaultCleanupGeneration() {
    AudioSessionCoordinator coordinator;
    const auto first = coordinator.Begin(AudioSessionMode::kLocalRecording);
    assert(coordinator.Fail(first, false));
    assert(!coordinator.BeginFaultCleanup(first.generation + 1));
    const auto cleanup = coordinator.BeginFaultCleanup(first.generation);
    assert(cleanup && coordinator.Fail(cleanup, false));
    assert(!coordinator.BeginFaultCleanup(first.generation));
    const auto retry = coordinator.BeginFaultCleanup(cleanup.generation);
    assert(retry && coordinator.Complete(retry));
    assert(!coordinator.BlocksSleep());
}

int main() {
    TestOwnerAndWorkerRelease();
    TestExpiredRequestCannotCloseLaterSession();
    TestStaleGenerationAndWorkerPoll();
    TestTimeoutDuringOwnerCloseStaysFailed();
    TestFaultCleanupGeneration();
    std::cout << "5 assistant transport handoff tests passed\n";
}
