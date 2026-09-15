#include "watch_ancs_session.h"
#include <algorithm>
#include <array>

bool WatchAncsSession::HasPendingRequests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == WatchAncsState::kReady &&
        (pending_.has_value() || !queued_.empty() || !queued_apps_.empty());
}
namespace {
uint32_t UInt32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}
void AppendUid(std::vector<uint8_t>& bytes, uint32_t uid) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<uint8_t>(uid >> shift));
}

// ANCS text is UTF-8, not NUL-terminated. Preserve codepoints and normalize C0
// controls before handing strings to label APIs. Only a byte-limited title or
// message may end in an incomplete codepoint at its negotiated limit.
bool Text(const uint8_t* bytes, size_t size, bool limited, std::string& text) {
    text.clear();
    for (size_t i = 0; i < size;) {
        const uint8_t first = bytes[i];
        if (first < 0x80) {
            text.push_back(first < 0x20 && first != '\n' && first != '\t' ? ' ' : static_cast<char>(first));
            ++i; continue;
        }
        const size_t count = first >= 0xc2 && first <= 0xdf ? 2 :
            first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        if (!count) return false;
        const size_t present = std::min(count, size - i);
        for (size_t n = 1; n < present; ++n) {
            if ((bytes[i + n] & 0xc0) != 0x80) return false;
        }
        if (present > 1 && ((first == 0xe0 && bytes[i + 1] < 0xa0) || (first == 0xed && bytes[i + 1] >= 0xa0) ||
            (first == 0xf0 && bytes[i + 1] < 0x90) || (first == 0xf4 && bytes[i + 1] >= 0x90))) return false;
        if (count > size - i) return limited;
        text.append(reinterpret_cast<const char*>(bytes + i), count);
        i += count;
    }
    return true;
}
}

uint64_t WatchAncsSession::NextSerialLocked() {
    if (++serial_ == 0) ++serial_;
    return serial_;
}
void WatchAncsSession::ResetLocked() {
    notifications_.clear(); queued_.clear(); pending_.reset(); session_ = 0;
    queued_apps_.clear(); apps_.clear();
}
void WatchAncsSession::SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled && state_ != WatchAncsState::kDisabled) return;
    ResetLocked(); error_ = WatchAncsError::kNone;
    state_ = enabled ? WatchAncsState::kDisconnected : WatchAncsState::kDisabled;
}
uint64_t WatchAncsSession::BeginSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == WatchAncsState::kDisabled) return 0;
    ResetLocked(); error_ = WatchAncsError::kNone;
    state_ = WatchAncsState::kAuthorizing;
    session_ = NextSerialLocked();
    return session_;
}
bool WatchAncsSession::Authorized(uint64_t session) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!session || session != session_ || state_ != WatchAncsState::kAuthorizing) return false;
    state_ = WatchAncsState::kReady; return true;
}
void WatchAncsSession::EndSession(uint64_t session) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!session || session != session_) return;
    ResetLocked(); state_ = WatchAncsState::kDisconnected; error_ = WatchAncsError::kNone;
}
bool WatchAncsSession::ReadyLocked(uint64_t session) const {
    return session && session == session_ && state_ == WatchAncsState::kReady;
}
bool WatchAncsSession::FailLocked(WatchAncsError error) {
    ResetLocked(); state_ = WatchAncsState::kFailed; error_ = error; return false;
}
bool WatchAncsSession::CheckTimeoutLocked(uint64_t now_ms) {
    if (pending_ && (now_ms < pending_->started_ms || now_ms - pending_->started_ms >= kRequestTimeoutMs))
        return FailLocked(WatchAncsError::kTimeout);
    return true;
}

bool WatchAncsSession::NotificationSource(uint64_t session, const uint8_t* bytes, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ReadyLocked(session)) return false;
    if (!bytes || size != 8 || bytes[0] > 2) return FailLocked(WatchAncsError::kProtocol);
    const auto uid = UInt32(bytes + 4);
    auto found = std::find_if(notifications_.begin(), notifications_.end(), [uid](const auto& item) { return item.uid == uid; });
    if (found != notifications_.end()) notifications_.erase(found);
    queued_.erase(std::remove(queued_.begin(), queued_.end(), uid), queued_.end());
    if (bytes[0] == 2) return true;
    if (notifications_.size() == kMaxNotifications) {
        const auto evicted = notifications_.back().uid;
        notifications_.pop_back();
        queued_.erase(std::remove(queued_.begin(), queued_.end(), evicted), queued_.end());
    }
    WatchPhoneNotification item;
    item.uid = uid; item.revision = NextSerialLocked(); item.flags = bytes[1];
    item.category = bytes[2] < 12 ? bytes[2] : 0;
    notifications_.push_front(std::move(item)); queued_.push_back(uid);
    return true;
}

std::optional<WatchAncsRequest> WatchAncsSession::NextRequest(uint64_t session, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ReadyLocked(session) || !CheckTimeoutLocked(now_ms) || pending_) return {};
    if (queued_.empty()) {
        while (!queued_apps_.empty()) {
            auto app = std::move(queued_apps_.front()); queued_apps_.pop_front();
            const bool retained = std::any_of(notifications_.begin(), notifications_.end(), [&app](const auto& item) {
                return item.details_ready && item.app_identifier == app;
            });
            if (!retained) continue;
            pending_.emplace();
            pending_->app_identifier = std::move(app);
            pending_->request = NextSerialLocked(); pending_->started_ms = now_ms;
            pending_->bytes.reserve(kMaxResponseBytes);
            WatchAncsRequest request{session_, pending_->request, 0, {1}};
            request.bytes.insert(request.bytes.end(), pending_->app_identifier.begin(), pending_->app_identifier.end());
            request.bytes.insert(request.bytes.end(), {0, 0});
            return request;
        }
        return {};
    }
    const auto uid = queued_.front(); queued_.pop_front();
    const auto found = std::find_if(notifications_.begin(), notifications_.end(), [uid](const auto& item) { return item.uid == uid; });
    if (found == notifications_.end()) return {};
    pending_.emplace();
    pending_->uid = uid; pending_->revision = found->revision;
    pending_->request = NextSerialLocked(); pending_->started_ms = now_ms;
    pending_->bytes.reserve(kMaxResponseBytes);
    WatchAncsRequest request{session_, pending_->request, uid, {0}};
    AppendUid(request.bytes, uid);
    // App identifier, title (120 bytes), message (512 bytes), date.
    const uint8_t attributes[] = {0, 1, 120, 0, 3, 0, 2, 5};
    request.bytes.insert(request.bytes.end(), std::begin(attributes), std::end(attributes));
    return request;
}

bool WatchAncsSession::RequestWritten(uint64_t session, uint64_t request, WatchAncsWriteResult result, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ReadyLocked(session) || !pending_ || !request || pending_->request != request || !CheckTimeoutLocked(now_ms)) return false;
    if (result == WatchAncsWriteResult::kTransportFailure) return FailLocked(WatchAncsError::kWrite);
    if (result == WatchAncsWriteResult::kRejected) {
        // ANCS errors A0-A3 promise no Data Source reply. They are not an I/O
        // timeout: retire this request without discarding unrelated notifications.
        if (pending_->acknowledged || !pending_->bytes.empty()) return FailLocked(WatchAncsError::kProtocol);
        if (pending_->app_identifier.empty()) {
            const auto found = std::find_if(notifications_.begin(), notifications_.end(), [this](const auto& item) {
                return item.uid == pending_->uid && item.revision == pending_->revision;
            });
            if (found != notifications_.end()) notifications_.erase(found);
        }
        pending_.reset(); return true;
    }
    pending_->acknowledged = true;
    PublishLocked(); return true;
}

bool WatchAncsSession::DecodeLocked() {
    if (!pending_->app_identifier.empty()) return DecodeAppLocked();
    const auto& bytes = pending_->bytes;
    if (bytes.size() < 5) return true;
    if (bytes[0] != 0 || UInt32(bytes.data() + 1) != pending_->uid) return FailLocked(WatchAncsError::kProtocol);
    size_t offset = 5;
    const std::array<uint8_t, 4> attributes{0, 1, 3, 5};
    const std::array<size_t, 4> limits{255, 120, 512, 15};
    WatchPhoneNotification decoded;
    const std::array<std::string*, 4> fields{&decoded.app_identifier, &decoded.title, &decoded.message, &decoded.date};
    for (size_t index = 0; index < attributes.size(); ++index) {
        if (bytes.size() - offset < 3) return true;
        const size_t length = static_cast<size_t>(bytes[offset + 1]) | (static_cast<size_t>(bytes[offset + 2]) << 8);
        if (bytes[offset] != attributes[index] || length > limits[index]) return FailLocked(WatchAncsError::kProtocol);
        offset += 3;
        if (bytes.size() - offset < length) return true;
        if (index == 0 && std::any_of(bytes.begin() + offset, bytes.begin() + offset + length,
                [](uint8_t byte) { return byte < 0x20 || byte == 0x7f; }))
            return FailLocked(WatchAncsError::kProtocol);
        if (!Text(bytes.data() + offset, length, (index == 1 || index == 2) && length == limits[index], *fields[index]))
            return FailLocked(WatchAncsError::kProtocol);
        offset += length;
    }
    if (offset != bytes.size()) return FailLocked(WatchAncsError::kProtocol);
    pending_->decoded = std::move(decoded);
    return true;
}

bool WatchAncsSession::DecodeAppLocked() {
    const auto& bytes = pending_->bytes;
    const auto& app = pending_->app_identifier;
    if (bytes.front() != 1) return FailLocked(WatchAncsError::kProtocol);
    for (size_t i = 1; i < std::min(bytes.size(), app.size() + 2); ++i) {
        if (bytes[i] != (i <= app.size() ? static_cast<uint8_t>(app[i - 1]) : 0))
            return FailLocked(WatchAncsError::kProtocol);
    }
    const size_t offset = app.size() + 2;
    if (bytes.size() < offset + 3) return true;
    const size_t length = static_cast<size_t>(bytes[offset + 1]) | (static_cast<size_t>(bytes[offset + 2]) << 8);
    // App attributes have no requested-length field. Bound the wire response,
    // rejecting overflow rather than letting it spill into the next request.
    if (bytes[offset] != 0 || length > 512) return FailLocked(WatchAncsError::kProtocol);
    if (bytes.size() < offset + 3 + length) return true;
    if (bytes.size() != offset + 3 + length) return FailLocked(WatchAncsError::kProtocol);
    WatchPhoneNotification decoded;
    if (!Text(bytes.data() + offset + 3, length, false, decoded.app_name))
        return FailLocked(WatchAncsError::kProtocol);
    pending_->decoded = std::move(decoded);
    return true;
}

void WatchAncsSession::PublishLocked() {
    if (!pending_ || !pending_->acknowledged || !pending_->decoded) return;
    if (!pending_->app_identifier.empty()) {
        const auto app = pending_->app_identifier;
        const bool retained = std::any_of(notifications_.begin(), notifications_.end(), [&app](const auto& item) {
            return item.details_ready && item.app_identifier == app;
        });
        if (retained) {
            apps_.erase(std::remove_if(apps_.begin(), apps_.end(), [&app](const auto& item) { return item.first == app; }), apps_.end());
            if (apps_.size() == kMaxNotifications) apps_.pop_front();
            apps_.emplace_back(app, std::move(pending_->decoded->app_name));
            for (auto& item : notifications_) {
                if (item.details_ready && item.app_identifier == app) item.app_name = apps_.back().second;
            }
        }
        pending_.reset(); return;
    }
    const auto found = std::find_if(notifications_.begin(), notifications_.end(), [this](const auto& item) {
        return item.uid == pending_->uid && item.revision == pending_->revision;
    });
    if (found != notifications_.end()) {
        auto& details = *pending_->decoded;
        found->app_identifier = std::move(details.app_identifier); found->title = std::move(details.title);
        found->message = std::move(details.message); found->date = std::move(details.date);
        found->details_ready = true;
        const auto app = std::find_if(apps_.begin(), apps_.end(), [&found](const auto& item) {
            return item.first == found->app_identifier;
        });
        if (app != apps_.end()) found->app_name = app->second;
    }
    pending_.reset();
}

bool WatchAncsSession::RequestAppName(uint64_t session, uint32_t uid, uint64_t revision) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ReadyLocked(session)) return false;
    const auto found = std::find_if(notifications_.begin(), notifications_.end(), [=](const auto& item) {
        return item.uid == uid && item.revision == revision && item.details_ready && !item.app_identifier.empty();
    });
    if (found == notifications_.end()) return false;
    const auto app = std::find_if(apps_.begin(), apps_.end(), [&found](const auto& item) { return item.first == found->app_identifier; });
    if (app != apps_.end()) { found->app_name = app->second; return true; }
    if ((pending_ && pending_->app_identifier == found->app_identifier) ||
        std::find(queued_apps_.begin(), queued_apps_.end(), found->app_identifier) != queued_apps_.end()) return true;
    if (queued_apps_.size() == kMaxNotifications) queued_apps_.pop_front();
    queued_apps_.push_back(found->app_identifier);
    return true;
}

bool WatchAncsSession::DataSource(uint64_t session, const uint8_t* bytes, size_t size, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ReadyLocked(session) || !CheckTimeoutLocked(now_ms)) return false;
    if (!pending_ || !bytes || !size || size > kMaxResponseBytes - pending_->bytes.size() || pending_->decoded)
        return FailLocked(WatchAncsError::kProtocol);
    pending_->bytes.insert(pending_->bytes.end(), bytes, bytes + size);
    if (!DecodeLocked()) return false;
    PublishLocked(); return true;
}

bool WatchAncsSession::MarkRead(uint64_t session, uint32_t uid, uint64_t revision) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ReadyLocked(session)) return false;
    const auto found = std::find_if(notifications_.begin(), notifications_.end(), [=](const auto& item) {
        return item.uid == uid && item.revision == revision && item.details_ready;
    });
    if (found == notifications_.end()) return false;
    found->read = true; return true;
}
bool WatchAncsSession::Clear(uint64_t session) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ReadyLocked(session)) return false;
    notifications_.clear(); queued_.clear();
    queued_apps_.clear(); apps_.clear();
    // The on-wire reply still needs draining before the next request. Its
    // revision no longer exists, so completion cannot restore cleared content.
    return true;
}
WatchAncsSnapshot WatchAncsSession::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {state_, error_, session_, {notifications_.begin(), notifications_.end()}};
}
WatchAncsState WatchAncsSession::State() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}
