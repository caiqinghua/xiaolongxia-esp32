#include "watch_android_notifications.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace {
constexpr uint64_t kMaxWireId = 0x7fffffffffffffffULL;
uint64_t Read(const uint8_t* bytes, size_t size) {
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i) value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    return value;
}
bool ValidText(const std::string& text) {
    size_t offset = 0;
    while (offset < text.size()) {
        uint32_t code = static_cast<uint8_t>(text[offset++]);
        size_t extra = 0;
        uint32_t minimum = 0;
        if (code >= 0xc2 && code <= 0xdf) { code &= 0x1f; extra = 1; minimum = 0x80; }
        else if (code >= 0xe0 && code <= 0xef) { code &= 0x0f; extra = 2; minimum = 0x800; }
        else if (code >= 0xf0 && code <= 0xf4) { code &= 7; extra = 3; minimum = 0x10000; }
        else if (code >= 0x80) return false;
        if (text.size() - offset < extra) return false;
        while (extra--) {
            const auto next = static_cast<uint8_t>(text[offset++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff) ||
            (code < 32 && code != 10) || (code >= 127 && code <= 159) ||
            (code >= 0x202a && code <= 0x202e) || (code >= 0x2066 && code <= 0x2069)) return false;
    }
    return true;
}
bool SameContent(const WatchAndroidNotification& a, const WatchAndroidNotification& b) {
    return a.uid == b.uid && a.posted_at_ms == b.posted_at_ms && a.package_name == b.package_name &&
        a.app_name == b.app_name && a.title == b.title && a.message == b.message;
}
}

void WatchAndroidNotifications::Reset() {
    session_ = wire_session_ = request_ = revision_ = received_at_ = started_at_ = 0;
    incoming_request_ = incoming_revision_ = 0;
    high_uid_ = cleared_through_ = 0;
    kind_ = 0; body_size_ = 0; have_snapshot_ = clear_pending_ = false;
    bytes_.clear(); entries_.clear();
}
void WatchAndroidNotifications::SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled && state_ != WatchAndroidNotificationState::kDisabled) return;
    Reset();
    state_ = enabled ? WatchAndroidNotificationState::kDisconnected : WatchAndroidNotificationState::kDisabled;
}
uint64_t WatchAndroidNotifications::BeginAuthorizedSession(uint64_t wire_session, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != WatchAndroidNotificationState::kDisconnected || wire_session == 0 || wire_session > kMaxWireId ||
        serial_ == std::numeric_limits<uint64_t>::max()) return 0;
    Reset();
    session_ = ++serial_; wire_session_ = wire_session; received_at_ = now_ms;
    state_ = WatchAndroidNotificationState::kReady;
    return session_;
}
void WatchAndroidNotifications::EndSession(uint64_t session) {
    std::lock_guard<std::mutex> lock(mutex_);
    // A failed stream remains quarantined until the native disconnect fence.
    if (session == 0 || session != serial_ || state_ == WatchAndroidNotificationState::kDisabled) return;
    Reset(); state_ = WatchAndroidNotificationState::kDisconnected;
}
WatchAndroidNotifications::ReceiveResult WatchAndroidNotifications::Fail() {
    Reset(); state_ = WatchAndroidNotificationState::kFailed;
    return {Result::kFailed, {}};
}
bool WatchAndroidNotifications::Expire(uint64_t now_ms) {
    if (state_ != WatchAndroidNotificationState::kReady) return false;
    if (now_ms < received_at_ || now_ms - received_at_ >= kSessionLeaseMs ||
        (!bytes_.empty() && (now_ms < started_at_ || now_ms - started_at_ >= kReceiveTimeoutMs))) {
        Fail(); return true;
    }
    return false;
}
bool WatchAndroidNotifications::Header() {
    const auto* b = bytes_.data();
    if (Read(b, 4) != 0x314e5741 || (b[4] != 1 && b[4] != 3) || Read(b + 5, 3) != 0 ||
        Read(b + 8, 8) != wire_session_) return false;
    kind_ = b[4]; incoming_request_ = Read(b + 16, 8); incoming_revision_ = Read(b + 24, 8);
    body_size_ = static_cast<size_t>(Read(b + 32, 4));
    if (incoming_request_ <= request_ || incoming_request_ > kMaxWireId || incoming_revision_ > kMaxWireId) return false;
    if (kind_ == 3) return have_snapshot_ && body_size_ == 0 && incoming_revision_ == revision_;
    return body_size_ >= 1 && body_size_ <= kMaxBodyBytes && (!have_snapshot_ || incoming_revision_ > revision_);
}
bool WatchAndroidNotifications::Decode(std::vector<WatchAndroidNotification>& decoded) {
    size_t pos = kHeaderBytes;
    const size_t count = bytes_[pos++];
    if (count > kMaxEntries) return false;
    auto text = [&](std::string& result, size_t limit) {
        if (bytes_.size() - pos < 2) return false;
        const auto length = static_cast<size_t>(Read(bytes_.data() + pos, 2)); pos += 2;
        if (length > limit || length > bytes_.size() - pos) return false;
        result.assign(reinterpret_cast<const char*>(bytes_.data() + pos), length); pos += length;
        return ValidText(result);
    };
    for (size_t i = 0; i < count; ++i) {
        if (bytes_.size() - pos < 12) return false;
        WatchAndroidNotification item;
        item.uid = static_cast<uint32_t>(Read(bytes_.data() + pos, 4)); pos += 4;
        item.posted_at_ms = Read(bytes_.data() + pos, 8); pos += 8;
        if (!item.uid || item.posted_at_ms > kMaxWireId ||
            std::any_of(decoded.begin(), decoded.end(), [&](const auto& entry) { return entry.uid == item.uid; })) return false;
        if (!text(item.package_name, 255) || item.package_name.empty() ||
            !std::all_of(item.package_name.begin(), item.package_name.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
            }) || !text(item.app_name, 120) || !text(item.title, 120) || !text(item.message, 512)) return false;
        item.revision = incoming_revision_ + 1;
        decoded.push_back(std::move(item));
    }
    return pos == bytes_.size();
}
WatchAndroidNotifications::ReceiveResult WatchAndroidNotifications::Apply(uint64_t now_ms) {
    if (kind_ == 1) {
        std::vector<WatchAndroidNotification> decoded;
        if (!Decode(decoded)) return Fail();
        for (auto& item : decoded) {
            high_uid_ = std::max(high_uid_, item.uid);
            const auto old = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) { return SameContent(entry, item); });
            if (old != entries_.end()) { item.read = old->read; item.revision = old->revision; }
        }
        if (clear_pending_) cleared_through_ = high_uid_;
        decoded.erase(std::remove_if(decoded.begin(), decoded.end(), [&](const auto& entry) {
            return entry.uid <= cleared_through_;
        }), decoded.end());
        entries_ = std::move(decoded); have_snapshot_ = true;
    }
    request_ = incoming_request_; revision_ = incoming_revision_; received_at_ = now_ms;
    std::vector<uint8_t> ack(bytes_.begin(), bytes_.begin() + kHeaderBytes);
    ack[4] = 2; ack[32] = 1; ack[33] = ack[34] = ack[35] = 0; ack.push_back(0);
    bytes_.clear(); clear_pending_ = false;
    return {Result::kApplied, std::move(ack)};
}
WatchAndroidNotifications::ReceiveResult WatchAndroidNotifications::Receive(
    uint64_t session, const uint8_t* bytes, size_t size, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!session || session != session_ || state_ != WatchAndroidNotificationState::kReady) return {Result::kIgnored, {}};
    if (Expire(now_ms)) return {Result::kFailed, {}};
    // One ATT value cannot carry more than 512 bytes. Do not accept unbounded
    // callback payloads even if their claimed snapshot size would be valid.
    if (!bytes || !size || size > 512) return Fail();
    if (bytes_.empty()) started_at_ = now_ms;
    size_t offset = 0;
    if (bytes_.size() < kHeaderBytes) {
        const auto take = std::min(size, kHeaderBytes - bytes_.size());
        bytes_.insert(bytes_.end(), bytes, bytes + take); offset += take;
        if (bytes_.size() < kHeaderBytes) return {Result::kReceiving, {}};
        if (!Header()) return Fail();
        bytes_.reserve(kHeaderBytes + body_size_);
    }
    const auto remaining = kHeaderBytes + body_size_ - bytes_.size();
    if (size - offset > remaining) return Fail();
    bytes_.insert(bytes_.end(), bytes + offset, bytes + size);
    if (bytes_.size() == kHeaderBytes + body_size_) return Apply(now_ms);
    return {Result::kReceiving, {}};
}
bool WatchAndroidNotifications::Clear(uint64_t session, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Expire(now_ms) || !session || session != session_ || state_ != WatchAndroidNotificationState::kReady) return false;
    entries_.clear(); cleared_through_ = high_uid_; clear_pending_ = !bytes_.empty();
    return true;
}
bool WatchAndroidNotifications::MarkRead(uint64_t session, uint32_t uid, uint64_t revision, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Expire(now_ms) || !session || session != session_ || state_ != WatchAndroidNotificationState::kReady) return false;
    const auto item = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
        return entry.uid == uid && entry.revision == revision;
    });
    if (item == entries_.end()) return false;
    item->read = true; return true;
}
bool WatchAndroidNotifications::Dismiss(uint64_t session, uint32_t uid, uint64_t revision, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Expire(now_ms) || !session || session != session_ || state_ != WatchAndroidNotificationState::kReady) return false;
    const auto item = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
        return entry.uid == uid && entry.revision == revision;
    });
    if (item == entries_.end()) return false;
    entries_.erase(item);
    return true;
}
WatchAndroidNotificationState WatchAndroidNotifications::Poll(uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_); Expire(now_ms); return state_;
}
WatchAndroidNotificationSnapshot WatchAndroidNotifications::Snapshot(uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_); Expire(now_ms); return {state_, session_, entries_};
}
