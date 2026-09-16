#include "watch_firmware_update.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits.h>
#include <limits>

#ifdef ESP_PLATFORM
#include "esp_pthread.h"
#endif

namespace watch_update {

Controller::Controller(Hooks hooks) : hooks_(std::move(hooks)) { state_.target = hooks_.target; }

Controller::~Controller() {
    Cancel(Read().attempt);
    if (!Join(0, true)) std::abort();
}

Snapshot Controller::Read() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool Controller::ExpireLocked() {
    if (state_.phase != Phase::kAvailable) return false;
    const auto now = hooks_.now_ms();
    if (available_at_ >= 0 && now >= available_at_ && now - available_at_ < kConfirmationMs) return false;
    envelope_.clear();
    state_.phase = Phase::kExpired;
    return true;
}

bool Controller::Check() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.active || state_.boot_selected || state_.commit_started || state_.phase == Phase::kResourceFault ||
            state_.attempt == std::numeric_limits<uint64_t>::max() || !hooks_.check || !hooks_.install ||
            !hooks_.acquire || !hooks_.release || !hooks_.now_ms) return false;
        const auto attempt = state_.attempt + 1;
        state_ = {};
        state_.target = hooks_.target;
        state_.attempt = attempt;
        state_.phase = Phase::kChecking;
        envelope_.clear();
    }
    return Start(Operation::kCheck);
}

bool Controller::Confirm(uint64_t attempt, uint32_t sequence) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ExpireLocked();
        if (state_.active || state_.phase != Phase::kAvailable || !attempt || state_.attempt != attempt ||
            sequence != state_.sequence || envelope_.empty() || !hooks_.pin_commit) return false;
        state_.phase = Phase::kInstalling;
    }
    return Start(Operation::kInstall);
}

bool Controller::Start(Operation operation) {
    pthread_attr_t attributes;
    const bool initialized = pthread_attr_init(&attributes) == 0;
    const bool configured = initialized &&
        pthread_attr_setstacksize(&attributes, std::max<size_t>(16384, PTHREAD_STACK_MIN)) == 0;
    const bool acquired = configured && hooks_.acquire(operation);
    bool created = false;
    if (acquired) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Immediate UI cancellation may have arrived while acquiring resources.
        if (!state_.cancelling && state_.phase != Phase::kCancelled) {
            operation_ = operation;
            finished_ = false;
            installed_ = {};
            checked_ = {};
            state_.active = true;
            created = pthread_create(&thread_, &attributes, Entry, this) == 0;
            if (!created) state_.active = false;
        }
    }
    if (initialized) pthread_attr_destroy(&attributes);
    if (created) return true;
    const bool released = !acquired || hooks_.release(operation, true);
    std::lock_guard<std::mutex> lock(mutex_);
    state_.phase = !released ? Phase::kResourceFault : state_.cancelling || state_.phase == Phase::kCancelled
        ? Phase::kCancelled : operation == Operation::kCheck ? Phase::kCheckFailed : Phase::kInstallFailed;
    envelope_.clear();
    return false;
}

bool Controller::Cancel(uint64_t attempt) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!attempt || attempt != state_.attempt || state_.commit_started || state_.boot_selected ||
        state_.phase == Phase::kResourceFault) return false;
    if (state_.phase != Phase::kChecking && state_.phase != Phase::kAvailable &&
        state_.phase != Phase::kInstalling) return false;
    state_.cancelling = true;
    if (!state_.active) {
        state_.phase = Phase::kCancelled;
        envelope_.clear();
    }
    return true;
}

void* Controller::Entry(void* context) {
    auto& self = *static_cast<Controller*>(context);
    const Cancelled cancelled = [&] {
        std::lock_guard<std::mutex> lock(self.mutex_);
        return self.state_.cancelling;
    };
    if (self.operation_ == Operation::kCheck) {
        self.checked_ = self.hooks_.check(cancelled);
    } else {
        const Progress progress = [&](int value, size_t speed) {
            std::lock_guard<std::mutex> lock(self.mutex_);
            // Only a successful install result, not a callback, proves completion.
            self.state_.progress = std::max(self.state_.progress, static_cast<unsigned>(std::clamp(value, 0, 99)));
            self.state_.bytes_per_second = speed;
        };
        const Cancelled before_commit = [&] {
            std::lock_guard<std::mutex> lock(self.mutex_);
            if (self.state_.cancelling || self.state_.commit_started) return false;
            if (!self.hooks_.pin_commit()) return false;
            self.state_.commit_started = true;
            self.state_.phase = Phase::kCommitting;
            return true;
        };
        self.installed_ = self.hooks_.install(self.envelope_, progress, cancelled, before_commit);
    }
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        self.finished_ = true;
    }
    self.completion_.notify_all();
    return nullptr;
}

bool Controller::Join(uint32_t timeout_ms, bool forever) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!state_.active) return true;
        if (forever) completion_.wait(lock, [&] { return finished_; });
        else if (!completion_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return finished_; })) return false;
    }
    if (pthread_join(thread_, nullptr) != 0) return false;
    const bool io_released = operation_ == Operation::kCheck || installed_.resources_released;
    const bool owner_released = hooks_.release(operation_, io_released);
    const bool released = io_released && owner_released;
    std::lock_guard<std::mutex> lock(mutex_);
    state_.active = false;
    if (operation_ == Operation::kInstall) {
        state_.boot_selected = installed_.installed;
        state_.phase = installed_.installed ? Phase::kRebootRequired : state_.commit_started ? Phase::kCommitUncertain
            : state_.cancelling ? Phase::kCancelled : Phase::kInstallFailed;
        if (installed_.installed) state_.progress = 100;
    } else if (state_.cancelling) {
        state_.phase = Phase::kCancelled;
    } else {
        using firmware_release::CheckStatus;
        state_.check_status = checked_.status;
        switch (checked_.status) {
            case CheckStatus::kAvailable:
                available_at_ = hooks_.now_ms();
                if (checked_.release && !checked_.envelope.empty() &&
                    (std::holds_alternative<asset_release::Verified>(*checked_.release) == (hooks_.target == Target::kModels)) &&
                    checked_.envelope.size() <= firmware_release::kMaximumManifestBytes && available_at_ >= 0) {
                    std::visit([&](const auto& release) {
                        state_.version = release.Version();
                        state_.sequence = release.Sequence();
                        state_.bytes = release.Size();
                    }, *checked_.release);
                    envelope_ = std::move(checked_.envelope);
                    state_.phase = Phase::kAvailable;
                } else state_.phase = Phase::kCheckFailed;
                break;
            case CheckStatus::kNoUpdate: state_.phase = Phase::kNoUpdate; break;
            case CheckStatus::kNotConfigured: state_.phase = Phase::kNotConfigured; break;
            case CheckStatus::kCancelled: state_.phase = Phase::kCancelled; break;
            default: state_.phase = Phase::kCheckFailed; break;
        }
    }
    if (!released) state_.phase = Phase::kResourceFault;
    if (state_.phase != Phase::kAvailable) envelope_.clear();
    checked_ = {};
    return true;
}

bool Controller::Poll() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!state_.active) return ExpireLocked();
    }
    return Join(0);
}

bool Controller::StopWithin(uint32_t timeout_ms) {
    Cancel(Read().attempt);
    if (!Join(timeout_ms)) return false;
    const auto snapshot = Read();
    return !snapshot.commit_started && !snapshot.boot_selected && snapshot.phase != Phase::kResourceFault;
}

}  // namespace watch_update
