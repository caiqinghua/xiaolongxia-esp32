#include "watch_voice_task.h"
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <limits.h>

#ifdef ESP_PLATFORM
#include "esp_pthread.h"
#endif

namespace watch_voice {

RequestTask::~RequestTask() {
    Cancel();
    std::lock_guard<std::mutex> lock(owner_mutex_);
    // A runtime join failure must not destroy memory still owned by a thread.
    if (!JoinLocked(0, true)) std::abort();
}

bool RequestTask::Start(Work work, const std::function<bool()>& acquire, std::function<void()> release) {
    std::unique_lock<std::mutex> lock(owner_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    if (active_ || !work || !acquire || !release) return false;
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return false;
    // ESP-IDF measures pthread stack size in bytes. TLS and parsing run here,
    // not on Application's stack. Host platforms may require a larger minimum.
    const size_t stack_size = std::max<size_t>(16384, PTHREAD_STACK_MIN);
    const bool configured = pthread_attr_setstacksize(&attributes, stack_size) == 0;
    if (!configured || !acquire()) {
        pthread_attr_destroy(&attributes);
        return false;
    }
    request_epoch_ = epoch_.load();
    result_ = {};
    status_ = Result::kCancelled;
    finished_ = false;
    work_ = std::move(work);
    release_ = std::move(release);
    const auto error = pthread_create(&thread_, &attributes, Entry, this);
    pthread_attr_destroy(&attributes);
    if (error != 0) {
        work_ = {};
        auto release_failed = std::move(release_);
        release_failed();
        return false;
    }
    active_ = true;
    return true;
}

void* RequestTask::Entry(void* context) {
    auto& task = *static_cast<RequestTask*>(context);
    {
        const Client::Cancelled cancelled = [&task]() {
            return task.epoch_.load() != task.request_epoch_;
        };
        task.status_ = task.work_(cancelled, task.result_);
        if (cancelled()) {
            task.status_ = Result::kCancelled;
            task.result_ = {};
        }
        // Destroy request captures (including credentials) before completion.
        task.work_ = {};
    }
    {
        std::lock_guard<std::mutex> lock(task.completion_mutex_);
        task.finished_ = true;
    }
    task.completion_.notify_all();
    return nullptr;
}

void RequestTask::Cancel() { epoch_.fetch_add(1); }

bool RequestTask::Active() const {
    return active_.load();
}

bool RequestTask::JoinLocked(uint32_t timeout_ms, bool wait_forever) {
    if (!active_) return true;
    {
        std::unique_lock<std::mutex> completion_lock(completion_mutex_);
        if (wait_forever) completion_.wait(completion_lock, [this]() { return finished_; });
        else if (!completion_.wait_for(completion_lock, std::chrono::milliseconds(timeout_ms),
                                        [this]() { return finished_; })) return false;
    }
    // finished_ means all HTTP handles and work captures are retired. Joining
    // also waits for the pthread runtime to finish, before releasing power.
    if (pthread_join(thread_, nullptr) != 0) return false;
    active_ = false;
    auto release = std::move(release_);
    release();
    return true;
}

bool RequestTask::Poll(Result& status, Response& result) {
    std::unique_lock<std::mutex> lock(owner_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    if (!active_ || !JoinLocked(0)) return false;
    status = epoch_.load() == request_epoch_ ? status_ : Result::kCancelled;
    result = status == Result::kOk ? std::move(result_) : Response{};
    result_ = {};
    return true;
}

bool RequestTask::StopWithin(uint32_t timeout_ms) {
    Cancel();
    std::unique_lock<std::mutex> lock(owner_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    if (!JoinLocked(timeout_ms)) return false;
    result_ = {};
    return true;
}

}  // namespace watch_voice
