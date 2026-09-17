#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct WatchAndroidNotification {
    uint32_t uid = 0;
    uint64_t revision = 0, posted_at_ms = 0;
    bool read = false;
    std::string package_name, app_name, title, message;
};

enum class WatchAndroidNotificationState { kDisabled, kDisconnected, kReady, kFailed };
struct WatchAndroidNotificationSnapshot {
    WatchAndroidNotificationState state = WatchAndroidNotificationState::kDisabled;
    uint64_t session = 0;
    std::vector<WatchAndroidNotification> entries;
    // Set only by the native adapter after validating the current secure peer.
    uint64_t peer_lease = 0;
};

// Bounded application protocol, not a GATT server or an authentication mechanism.
// The native adapter must supply a fresh authorized physical link, preserve its
// generation on callbacks, poll deadlines, and disconnect after failure/revoke.
class WatchAndroidNotifications {
public:
    static constexpr size_t kHeaderBytes = 36, kMaxBodyBytes = 20541, kMaxEntries = 20;
    static constexpr uint64_t kReceiveTimeoutMs = 5000, kSessionLeaseMs = 30000;
    enum class Result { kIgnored, kReceiving, kApplied, kFailed };
    struct ReceiveResult { Result result; std::vector<uint8_t> acknowledgement; };

    void SetEnabled(bool enabled);
    uint64_t BeginAuthorizedSession(uint64_t wire_session, uint64_t now_ms);
    void EndSession(uint64_t session);
    ReceiveResult Receive(uint64_t session, const uint8_t* bytes, size_t size, uint64_t now_ms);
    bool Clear(uint64_t session, uint64_t now_ms);
    bool MarkRead(uint64_t session, uint32_t uid, uint64_t revision, uint64_t now_ms);
    bool Dismiss(uint64_t session, uint32_t uid, uint64_t revision, uint64_t now_ms);
    WatchAndroidNotificationState Poll(uint64_t now_ms);
    WatchAndroidNotificationSnapshot Snapshot(uint64_t now_ms);

private:
    void Reset();
    ReceiveResult Fail();
    bool Expire(uint64_t now_ms);
    bool Header();
    bool Decode(std::vector<WatchAndroidNotification>& decoded);
    ReceiveResult Apply(uint64_t now_ms);
    std::mutex mutex_;
    WatchAndroidNotificationState state_ = WatchAndroidNotificationState::kDisabled;
    uint64_t serial_ = 0, session_ = 0, wire_session_ = 0;
    uint64_t request_ = 0, revision_ = 0, received_at_ = 0, started_at_ = 0;
    uint64_t incoming_request_ = 0, incoming_revision_ = 0;
    uint32_t high_uid_ = 0, cleared_through_ = 0;
    size_t body_size_ = 0;
    uint8_t kind_ = 0;
    bool have_snapshot_ = false, clear_pending_ = false;
    std::vector<uint8_t> bytes_;
    std::vector<WatchAndroidNotification> entries_;
};
