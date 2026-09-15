#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

struct WatchBleLinkParameters {
    uint16_t minimum_interval;
    uint16_t maximum_interval;
    uint16_t latency;
    uint16_t supervision_timeout;
};

// Connection intervals use 1.25 ms units; supervision timeout uses 10 ms units.
// This policy requests parameters, never assumes the central accepted them.
class WatchBleLinkPolicy {
public:
    using Address = std::array<uint8_t, 6>;
    using Submit = std::function<bool(const Address&, const WatchBleLinkParameters&)>;
    static constexpr WatchBleLinkParameters kActive{24, 36, 0, 400};
    static constexpr WatchBleLinkParameters kIdle{36, 48, 4, 400};
    static constexpr int64_t kInitialDelayMs = 5000;
    static constexpr int64_t kActivityMs = 15000;
    static constexpr int64_t kRetryMs = 5000;
    static constexpr int64_t kResponseMs = 10000;
    static constexpr unsigned kMaxAttempts = 2;

    void Connected(const uint8_t* address, int64_t now_ms);
    void Disconnected();
    void Activity(int64_t now_ms);
    void Poll(int64_t now_ms, bool busy, const Submit& submit);
    bool Updated(const uint8_t* address, uint16_t minimum_interval, uint16_t maximum_interval,
                 bool success, int64_t now_ms);

private:
    bool CheckClock(int64_t now_ms);
    void Reset();
    std::mutex mutex_;
    Address address_{};
    bool connected_ = false;
    int64_t last_ms_ = 0;
    int64_t active_until_ms_ = 0;
    int64_t next_request_ms_ = 0;
    int64_t response_deadline_ms_ = 0;
    std::optional<bool> desired_active_;
    std::optional<bool> completed_active_;
    std::optional<bool> pending_active_;
    unsigned attempts_ = 0;
};
