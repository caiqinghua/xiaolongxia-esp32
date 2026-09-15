#pragma once

#include "firmware_release.h"
#include "asset_update_client.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <pthread.h>
#include <variant>

namespace watch_update {

enum class Phase { kIdle, kChecking, kAvailable, kNoUpdate, kCheckFailed, kNotConfigured,
    kCancelled, kExpired, kInstalling, kCommitting, kInstallFailed, kCommitUncertain,
    kRebootRequired, kResourceFault };
enum class Operation { kCheck, kInstall };
enum class Target { kFirmware, kModels };

struct CheckResult {
    firmware_release::CheckStatus status = firmware_release::CheckStatus::kNotConfigured;
    std::optional<std::variant<firmware_release::Verified, asset_release::Verified>> release;
    std::string envelope;
    CheckResult() = default;
    CheckResult(firmware_release::CheckResult result) : status(result.status), envelope(std::move(result.envelope)) {
        if (result.release) release = std::move(*result.release);
    }
    CheckResult(asset_update::CheckResult result) : status(result.status), envelope(std::move(result.envelope)) {
        if (result.release) release = std::move(*result.release);
    }
};

struct Snapshot {
    Target target = Target::kFirmware;
    Phase phase = Phase::kIdle;
    uint64_t attempt = 0;
    std::string version;
    uint32_t sequence = 0;
    size_t bytes = 0;
    unsigned progress = 0;
    size_t bytes_per_second = 0;
    firmware_release::CheckStatus check_status = firmware_release::CheckStatus::kNotConfigured;
    bool active = false;
    bool cancelling = false;
    bool boot_selected = false;
    bool commit_started = false;
};

// Start/confirm/poll/stop belong to one owner task. Cancel and Read may also be
// called from UI intent callbacks. Hooks must outlive the controller and never
// throw; only the owner acquires/releases resources, after the worker is joined.
class Controller {
public:
    using Cancelled = std::function<bool()>;
    using Progress = std::function<void(int, size_t)>;
    struct Hooks {
        std::function<CheckResult(const Cancelled&)> check;
        std::function<firmware_release::InstallResult(const std::string&, const Progress&, const Cancelled&, const Cancelled&)> install;
        std::function<bool(Operation)> acquire;
        // A false I/O release report must retain the coordinator fault/lease.
        std::function<bool(Operation, bool)> release;
        std::function<int64_t()> now_ms;
        // Runs on the worker under the controller mutex. Must atomically pin the
        // resource generation, never block or call back into this controller.
        std::function<bool()> pin_commit;
        Target target = Target::kFirmware;
    };
    explicit Controller(Hooks hooks);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;
    bool Check();
    bool Confirm(uint64_t attempt, uint32_t sequence);
    bool Cancel(uint64_t attempt);
    bool Poll();
    // True means safe handoff, not merely a joined worker. Commit and resource
    // faults return false even after join; Read().active reports worker lifetime.
    bool StopWithin(uint32_t timeout_ms);
    Snapshot Read() const;

private:
    static constexpr int64_t kConfirmationMs = 120000;
    Hooks hooks_;
    mutable std::mutex mutex_;
    std::condition_variable completion_;
    Snapshot state_;
    Operation operation_ = Operation::kCheck;
    pthread_t thread_{};
    bool finished_ = false;
    firmware_release::InstallResult installed_;
    int64_t available_at_ = -1;
    std::string envelope_;
    CheckResult checked_;
    bool Start(Operation operation);
    bool ExpireLocked();
    bool Join(uint32_t timeout_ms, bool forever = false);
    static void* Entry(void* context);
};

}  // namespace watch_update
