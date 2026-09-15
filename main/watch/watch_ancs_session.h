#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

enum class WatchAncsState { kDisabled, kDisconnected, kAuthorizing, kReady, kFailed };
enum class WatchAncsError { kNone, kProtocol, kTimeout, kWrite };
// kRejected is only for a confirmed ANCS A0-A3 ATT response, never a timeout,
// permission failure, disconnected peer, or ambiguous native write status.
enum class WatchAncsWriteResult { kAccepted, kRejected, kTransportFailure };

struct WatchPhoneNotification {
    uint32_t uid = 0;
    uint64_t revision = 0;
    uint8_t flags = 0, category = 0;
    bool details_ready = false, read = false;
    std::string app_identifier, app_name, title, message, date;
    // Android supplies UTC epoch milliseconds; ANCS keeps its local date string.
    uint64_t posted_at_ms = 0;
};

struct WatchAncsSnapshot {
    WatchAncsState state = WatchAncsState::kDisabled;
    WatchAncsError error = WatchAncsError::kNone;
    uint64_t session = 0;
    std::vector<WatchPhoneNotification> notifications;
};

struct WatchAncsRequest {
    uint64_t session = 0, request = 0;
    uint32_t uid = 0;
    std::vector<uint8_t> bytes;
};

// Protocol engine only. The platform adapter owns GATT discovery, encrypted
// bonding, user permission, subscriptions and callback-to-session association.
// On failure it must retire the old subscription before beginning another one.
class WatchAncsSession {
public:
    static constexpr size_t kMaxNotifications = 20;
    static constexpr size_t kMaxResponseBytes = 919;
    static constexpr uint64_t kRequestTimeoutMs = 5000;

    void SetEnabled(bool enabled);
    uint64_t BeginSession();
    bool Authorized(uint64_t session);
    void EndSession(uint64_t session);
    bool NotificationSource(uint64_t session, const uint8_t* bytes, size_t size);
    std::optional<WatchAncsRequest> NextRequest(uint64_t session, uint64_t now_ms);
    bool RequestWritten(uint64_t session, uint64_t request, WatchAncsWriteResult result, uint64_t now_ms);
    bool DataSource(uint64_t session, const uint8_t* bytes, size_t size, uint64_t now_ms);
    bool RequestAppName(uint64_t session, uint32_t uid, uint64_t revision);
    bool MarkRead(uint64_t session, uint32_t uid, uint64_t revision);
    bool Clear(uint64_t session);
    WatchAncsSnapshot Snapshot() const;
    WatchAncsState State() const;

private:
    struct Pending {
        uint64_t request = 0, revision = 0, started_ms = 0;
        uint32_t uid = 0;
        bool acknowledged = false;
        std::string app_identifier;
        std::vector<uint8_t> bytes;
        std::optional<WatchPhoneNotification> decoded;
    };
    bool ReadyLocked(uint64_t session) const;
    bool CheckTimeoutLocked(uint64_t now_ms);
    bool FailLocked(WatchAncsError error);
    bool DecodeLocked();
    bool DecodeAppLocked();
    void PublishLocked();
    void ResetLocked();
    mutable std::mutex mutex_;
    WatchAncsState state_ = WatchAncsState::kDisabled;
    WatchAncsError error_ = WatchAncsError::kNone;
    uint64_t session_ = 0, serial_ = 0;
    std::deque<WatchPhoneNotification> notifications_;
    std::deque<uint32_t> queued_;
    std::deque<std::string> queued_apps_;
    std::deque<std::pair<std::string, std::string>> apps_;
    std::optional<Pending> pending_;
    uint64_t NextSerialLocked();
};
