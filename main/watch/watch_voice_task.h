#pragma once

#include "watch_voice_cloud.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <pthread.h>
#include <variant>

namespace watch_voice {

using Response = std::variant<Bootstrap, Settings>;

// One joinable request, owned by Application. Cancellation does not release
// the caller's power reservation; only joining the actual worker does.
class RequestTask {
public:
    using Work = std::function<Result(const Client::Cancelled&, Response&)>;
    ~RequestTask();
    bool Start(Work work, const std::function<bool()>& acquire, std::function<void()> release);
    void Cancel();
    bool Active() const;
    bool Poll(Result& status, Response& result);
    bool StopWithin(uint32_t timeout_ms);

private:
    mutable std::mutex owner_mutex_;
    std::mutex completion_mutex_;
    std::condition_variable completion_;
    std::atomic<uint64_t> epoch_{0};
    uint64_t request_epoch_ = 0;
    pthread_t thread_{};
    std::atomic<bool> active_{false};
    bool finished_ = false;
    Work work_;
    std::function<void()> release_;
    Response result_;
    Result status_ = Result::kCancelled;
    static void* Entry(void* context);
    bool JoinLocked(uint32_t timeout_ms, bool wait_forever = false);
};

}  // namespace watch_voice
