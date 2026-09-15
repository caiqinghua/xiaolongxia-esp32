#include "assistant_transport_handoff.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

AssistantTransportHandoff::AssistantTransportHandoff(Dispatch dispatch, IsOwner is_owner,
    IsCurrent is_current, Close close)
    : dispatch_(std::move(dispatch)), is_owner_(std::move(is_owner)),
      is_current_(std::move(is_current)), close_(std::move(close)) {}

bool AssistantTransportHandoff::CloseWithin(uint64_t generation, uint32_t timeout_ms) const {
    if (!dispatch_ || !is_owner_ || !is_current_ || !close_ || !is_current_(generation)) return false;
    if (is_owner_()) {
        return close_(timeout_ms) && is_current_(generation);
    }
    if (timeout_ms == 0) return false;

    using Clock = std::chrono::steady_clock;
    struct Request {
        std::mutex mutex;
        std::condition_variable ready;
        bool finished = false;
        bool released = false;
    };
    auto request = std::make_shared<Request>();
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    dispatch_([request, deadline, generation, is_current = is_current_, close = close_]() {
        std::unique_lock<std::mutex> lock(request->mutex);
        const auto now = Clock::now();
        if (request->finished) return;
        if (now >= deadline || !is_current(generation)) {
            request->finished = true;
            request->ready.notify_all();
            return;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        lock.unlock();
        // No request mutex is held while a transport waits for its receive task.
        const bool released = close(static_cast<uint32_t>(remaining));
        lock.lock();
        if (!request->finished) {
            request->released = released && Clock::now() <= deadline && is_current(generation);
            request->finished = true;
            request->ready.notify_all();
        }
    });
    std::unique_lock<std::mutex> lock(request->mutex);
    if (!request->ready.wait_until(lock, deadline, [&]() { return request->finished; })) {
        // Queued work must not close a later session after the caller gives up.
        request->finished = true;
        return false;
    }
    return request->released;
}
