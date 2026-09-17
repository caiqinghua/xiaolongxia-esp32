#include "watch/watch_ancs_session.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
constexpr auto kAccepted = WatchAncsWriteResult::kAccepted;
constexpr auto kRejected = WatchAncsWriteResult::kRejected;
constexpr auto kTransportFailure = WatchAncsWriteResult::kTransportFailure;
constexpr uint32_t kUid = 0xfedcba98;
uint64_t Ready(WatchAncsSession& engine) {
    engine.SetEnabled(true);
    const auto session = engine.BeginSession();
    assert(session && engine.Authorized(session));
    assert(!engine.HasPendingRequests());
    return session;
}
Bytes Event(uint32_t uid, uint8_t type = 0) {
    return {type, 0x10, 4, 1, static_cast<uint8_t>(uid), static_cast<uint8_t>(uid >> 8),
        static_cast<uint8_t>(uid >> 16), static_cast<uint8_t>(uid >> 24)};
}
bool Notify(WatchAncsSession& engine, uint64_t session, uint32_t uid, uint8_t type = 0) {
    const auto bytes = Event(uid, type);
    return engine.NotificationSource(session, bytes.data(), bytes.size());
}
Bytes Reply(uint32_t uid = kUid, std::array<std::string, 4> fields = {
        "ai.audream.app", "Title", "Message", "20260914T103000"}) {
    Bytes bytes{0, static_cast<uint8_t>(uid), static_cast<uint8_t>(uid >> 8),
        static_cast<uint8_t>(uid >> 16), static_cast<uint8_t>(uid >> 24)};
    const uint8_t attributes[]{0, 1, 3, 5};
    for (size_t i = 0; i < fields.size(); ++i) {
        bytes.push_back(attributes[i]);
        bytes.push_back(static_cast<uint8_t>(fields[i].size()));
        bytes.push_back(static_cast<uint8_t>(fields[i].size() >> 8));
        bytes.insert(bytes.end(), fields[i].begin(), fields[i].end());
    }
    return bytes;
}
bool Receive(WatchAncsSession& engine, uint64_t session, const Bytes& bytes, uint64_t now = 101) {
    return engine.DataSource(session, bytes.data(), bytes.size(), now);
}
WatchAncsRequest Start(WatchAncsSession& engine, uint64_t session, uint32_t uid = kUid) {
    assert(Notify(engine, session, uid));
    assert(engine.HasPendingRequests());
    const auto request = engine.NextRequest(session, 100);
    assert(request && request->uid == uid && request->session == session);
    return *request;
}
void Ack(WatchAncsSession& engine, const WatchAncsRequest& request) {
    assert(engine.RequestWritten(request.session, request.request, kAccepted, 101));
}
void Failed(const WatchAncsSession& engine, WatchAncsError error) {
    const auto state = engine.Snapshot();
    assert(state.state == WatchAncsState::kFailed && state.error == error);
    assert(state.session == 0 && state.notifications.empty());
    assert(!engine.HasPendingRequests());
}

void TestAuthorizationAndSessionRetirement() {
    WatchAncsSession engine;
    assert(engine.Snapshot().state == WatchAncsState::kDisabled);
    assert(!engine.BeginSession() && !engine.Authorized(0));
    engine.SetEnabled(true);
    const auto first = engine.BeginSession();
    assert(first && !Notify(engine, first, kUid));
    assert(!engine.Authorized(first + 1) && !engine.NextRequest(first, 100));
    assert(engine.Authorized(first));
    auto request = Start(engine, first);
    Ack(engine, request);
    assert(Receive(engine, first, Reply()));
    engine.EndSession(first);
    assert(engine.Snapshot().notifications.empty());
    assert(engine.Snapshot().state == WatchAncsState::kDisconnected);
    const auto second = engine.BeginSession();
    assert(second && second != first && engine.Authorized(second));
    assert(!Notify(engine, first, kUid));
    assert(!Receive(engine, first, Reply()));
    assert(!engine.RequestWritten(first, request.request, kTransportFailure, 102));
    engine.EndSession(first);
    assert(engine.Snapshot().session == second);
    request = Start(engine, second, 0);
    Ack(engine, request);
    assert(Receive(engine, second, Reply(0)));
    assert(engine.Snapshot().notifications.front().details_ready);
    engine.SetEnabled(false);
    assert(engine.Snapshot().notifications.empty());
    assert(engine.Snapshot().state == WatchAncsState::kDisabled);
    assert(!Notify(engine, second, 0));
}

void TestWireRequestAndEverySplit() {
    const auto reply = Reply();
    for (size_t split = 1; split < reply.size(); ++split) {
        for (bool ack_first : {false, true}) {
            WatchAncsSession engine;
            const auto session = Ready(engine);
            const auto request = Start(engine, session);
            const Bytes expected{0, 0x98, 0xba, 0xdc, 0xfe, 0, 1, 120, 0, 3, 0, 2, 5};
            assert(request.bytes == expected);
            if (ack_first) Ack(engine, request);
            assert(engine.DataSource(session, reply.data(), split, 102));
            assert(!engine.Snapshot().notifications.front().details_ready);
            assert(engine.DataSource(session, reply.data() + split, reply.size() - split, 103));
            if (!ack_first) {
                assert(!engine.Snapshot().notifications.front().details_ready);
                Ack(engine, request);
            }
            const auto item = engine.Snapshot().notifications.front();
            assert(item.uid == kUid && item.flags == 0x10 && item.category == 4);
            assert(item.details_ready && item.title == "Title" && item.message == "Message");
            assert(item.app_identifier == "ai.audream.app" && item.date == "20260914T103000");
            assert(!engine.NextRequest(session, 104));
        }
    }
    WatchAncsSession engine;
    const auto session = Ready(engine);
    const auto request = Start(engine, session);
    Ack(engine, request);
    for (size_t i = 0; i < reply.size(); ++i) {
        assert(engine.DataSource(session, &reply[i], 1, 102));
        assert(engine.Snapshot().notifications.front().details_ready == (i + 1 == reply.size()));
    }
}

void TestInflightModificationRemovalAndClear() {
    for (unsigned mutation = 0; mutation < 4; ++mutation) {
        WatchAncsSession engine;
        const auto session = Ready(engine);
        auto request = Start(engine, session);
        const auto old_revision = engine.Snapshot().notifications.front().revision;
        Ack(engine, request);
        const auto reply = Reply();
        assert(engine.DataSource(session, reply.data(), 7, 102));
        if (mutation == 0) assert(Notify(engine, session, kUid, 1));
        if (mutation == 1) assert(Notify(engine, session, kUid, 2));
        if (mutation >= 2) assert(engine.Clear(session));
        if (mutation == 3) assert(Notify(engine, session, kUid));
        assert(!engine.NextRequest(session, 103));
        assert(engine.DataSource(session, reply.data() + 7, reply.size() - 7, 104));
        assert(!engine.MarkRead(session, kUid, old_revision));
        if (mutation == 1 || mutation == 2) {
            assert(engine.Snapshot().notifications.empty());
            assert(!engine.NextRequest(session, 105));
        } else {
            auto item = engine.Snapshot().notifications.front();
            assert(!item.details_ready && item.title.empty() && item.revision != old_revision);
            auto next = engine.NextRequest(session, 105);
            assert(next && next->request != request.request);
            assert(!engine.RequestWritten(session, request.request, kAccepted, 106));
            assert(engine.RequestWritten(session, next->request, kAccepted, 106));
            assert(Receive(engine, session, Reply(kUid, {"", "Updated", "", ""}), 107));
            item = engine.Snapshot().notifications.front();
            assert(item.title == "Updated" && item.details_ready && !item.read);
            assert(engine.MarkRead(session, kUid, item.revision));
            assert(engine.Snapshot().notifications.front().read);
        }
    }
}

void TestBoundedRetentionAndPendingEviction() {
    WatchAncsSession engine;
    const auto session = Ready(engine);
    const auto pending = Start(engine, session, 0);
    Ack(engine, pending);
    for (uint32_t uid = 1; uid < 101; ++uid) assert(Notify(engine, session, uid));
    const auto state = engine.Snapshot();
    assert(state.notifications.size() == WatchAncsSession::kMaxNotifications);
    assert(state.notifications.front().uid == 100 && state.notifications.back().uid == 81);
    assert(Receive(engine, session, Reply(0)));
    for (uint32_t uid = 81; uid <= 100; ++uid) {
        const auto request = engine.NextRequest(session, 100);
        assert(request && request->uid == uid);
        Ack(engine, *request);
        assert(Receive(engine, session, Reply(uid)));
    }
    assert(!engine.NextRequest(session, 102));
    for (unsigned i = 0; i < 1000; ++i) assert(Notify(engine, session, 100));
    auto request = engine.NextRequest(session, 100);
    assert(request && request->uid == 100);
    Ack(engine, *request);
    assert(Receive(engine, session, Reply(100)));
    assert(!engine.NextRequest(session, 103));
    assert(engine.Snapshot().notifications.size() == 20);
}

void TestGuardedLocalDismissal() {
    WatchAncsSession engine;
    const auto session = Ready(engine);
    const auto request = Start(engine, session);
    Ack(engine, request);
    assert(Receive(engine, session, Reply()));
    const auto item = engine.Snapshot().notifications.front();
    assert(!engine.Dismiss(session, item.uid, item.revision + 1));
    assert(engine.Dismiss(session, item.uid, item.revision));
    assert(engine.Snapshot().notifications.empty());
    assert(!engine.Dismiss(session, item.uid, item.revision));
}

void TestFailuresAndTimeoutsRetirePrivateData() {
    for (unsigned failure = 0; failure < 5; ++failure) {
        WatchAncsSession engine;
        const auto session = Ready(engine);
        const auto request = Start(engine, session);
        if (failure == 0) {
            assert(Receive(engine, session, Reply()));
            assert(!engine.RequestWritten(session, request.request, kTransportFailure, 102));
            Failed(engine, WatchAncsError::kWrite);
        } else {
            Ack(engine, request);
            const auto limit = 100 + WatchAncsSession::kRequestTimeoutMs;
            if (failure == 1) assert(!engine.NextRequest(session, limit));
            if (failure == 2) assert(!Receive(engine, session, Reply(), limit));
            if (failure == 3) assert(!engine.RequestWritten(session, request.request, kAccepted, limit));
            if (failure == 4) assert(!engine.NextRequest(session, 99));
            Failed(engine, WatchAncsError::kTimeout);
        }
        const auto next_session = engine.BeginSession();
        assert(next_session != session && engine.Authorized(next_session));
        const auto next_request = Start(engine, next_session);
        assert(!Receive(engine, session, Reply()));
        assert(!engine.RequestWritten(session, request.request, kAccepted, 104));
        Ack(engine, next_request);
        assert(Receive(engine, next_session, Reply()));
    }
}

void TestMalformedResponses() {
    std::vector<Bytes> malformed;
    auto bytes = Reply(); bytes[0] = 1; malformed.push_back(bytes);
    bytes = Reply(); bytes[1] ^= 1; malformed.push_back(bytes);
    bytes = Reply(); bytes[5] = 2; malformed.push_back(bytes);
    bytes = Reply(); bytes[6] = 0xff; bytes[7] = 0xff; malformed.push_back(bytes);
    bytes = Reply(); bytes.push_back(0); malformed.push_back(bytes);
    malformed.emplace_back(WatchAncsSession::kMaxResponseBytes + 1, 0);
    malformed.push_back(Reply(kUid, {std::string(256, 'a'), "", "", ""}));
    malformed.push_back(Reply(kUid, {"", std::string(121, 'a'), "", ""}));
    malformed.push_back(Reply(kUid, {"", "", std::string(513, 'a'), ""}));
    malformed.push_back(Reply(kUid, {"", "", "", std::string(16, 'a')}));
    for (const auto& reply : malformed) {
        WatchAncsSession engine;
        const auto session = Ready(engine);
        Ack(engine, Start(engine, session));
        assert(!Receive(engine, session, reply));
        Failed(engine, WatchAncsError::kProtocol);
    }
    for (size_t size = 0; size < 10; ++size) {
        if (size == 8) continue;
        WatchAncsSession engine;
        const auto session = Ready(engine);
        bytes.assign(size, 0);
        assert(!engine.NotificationSource(session, bytes.data(), size));
        Failed(engine, WatchAncsError::kProtocol);
    }
    WatchAncsSession engine;
    const auto session = Ready(engine);
    assert(!Receive(engine, session, Reply()));
    Failed(engine, WatchAncsError::kProtocol);
}

void TestTextLimitsAndInvalidUtf8() {
    const std::string unicode = "\xe9\x80\x9a\xe7\x9f\xa5 \xf0\x9f\x93\xb1";
    WatchAncsSession engine;
    auto session = Ready(engine);
    Ack(engine, Start(engine, session));
    assert(Receive(engine, session, Reply(kUid, {"", unicode, std::string("A\0B\rC\nD", 7), ""})));
    auto item = engine.Snapshot().notifications.front();
    assert(item.title == unicode && item.message == "A B C\nD");
    // A boundary cutting a valid multi-byte sequence drops only its partial suffix.
    session = engine.BeginSession(); assert(engine.Authorized(session));
    Ack(engine, Start(engine, session));
    const auto max_reply = Reply(kUid, {std::string(255, 'a'), std::string(118, 't') + "\xe9\x80",
        std::string(509, 'm') + "\xf0\x9f\x93", std::string(15, 'd')});
    assert(max_reply.size() == WatchAncsSession::kMaxResponseBytes);
    assert(Receive(engine, session, max_reply));
    item = engine.Snapshot().notifications.front();
    assert(item.title.size() == 118 && item.message.size() == 509);
    for (const auto& invalid : {"\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe9", "\x80", "\xe0\x80\x80"}) {
        session = engine.BeginSession(); assert(engine.Authorized(session));
        Ack(engine, Start(engine, session));
        assert(!Receive(engine, session, Reply(kUid, {"", invalid, "", ""})));
        Failed(engine, WatchAncsError::kProtocol);
    }
}

Bytes AppReply(const std::string& app = "ai.audream.app", const std::string& name = "Audream") {
    Bytes bytes{1};
    bytes.insert(bytes.end(), app.begin(), app.end());
    bytes.insert(bytes.end(), {0, 0, static_cast<uint8_t>(name.size()), static_cast<uint8_t>(name.size() >> 8)});
    bytes.insert(bytes.end(), name.begin(), name.end());
    return bytes;
}
WatchAncsRequest StartApp(WatchAncsSession& engine, uint64_t session) {
    Ack(engine, Start(engine, session));
    assert(Receive(engine, session, Reply()));
    const auto item = engine.Snapshot().notifications.front();
    assert(!engine.RequestAppName(session, kUid, item.revision + 1));
    assert(engine.RequestAppName(session, kUid, item.revision));
    const auto request = engine.NextRequest(session, 100);
    assert(request && request->bytes.front() == 1);
    return *request;
}

void TestAppNameWireFragmentsAndSessionCache() {
    const auto reply = AppReply();
    for (size_t split = 1; split < reply.size(); ++split) {
        WatchAncsSession engine;
        const auto session = Ready(engine);
        const auto request = StartApp(engine, session);
        const Bytes expected{1, 'a', 'i', '.', 'a', 'u', 'd', 'r', 'e', 'a', 'm', '.', 'a', 'p', 'p', 0, 0};
        assert(request.bytes == expected);
        assert(engine.DataSource(session, reply.data(), split, 102));
        assert(engine.DataSource(session, reply.data() + split, reply.size() - split, 103));
        assert(engine.Snapshot().notifications.front().app_name.empty());
        Ack(engine, request);
        const auto item = engine.Snapshot().notifications.front();
        assert(item.app_name == "Audream");
        assert(engine.RequestAppName(session, kUid, item.revision));
        assert(!engine.NextRequest(session, 104));
        // Later notifications from the same app use the cache without another write.
        Ack(engine, Start(engine, session, 0));
        assert(Receive(engine, session, Reply(0)));
        assert(engine.Snapshot().notifications.front().app_name == "Audream");
        engine.EndSession(session);
        const auto next_session = engine.BeginSession();
        assert(engine.Authorized(next_session));
        const auto next_request = StartApp(engine, next_session);
        assert(next_request.session != request.session);
        assert(!Receive(engine, session, reply));
        assert(engine.Snapshot().notifications.front().app_name.empty());
    }
}

void TestAppNameMalformedRepliesAndClearing() {
    std::vector<Bytes> malformed{AppReply("different.app"), AppReply("ai.audream.app", std::string(513, 'a')),
        AppReply("ai.audream.app", "\xe9")};
    auto reply = AppReply(); reply.push_back(0); malformed.push_back(reply);
    reply = AppReply(); reply[0] = 0; malformed.push_back(reply);
    reply = AppReply(); reply[16] = 3; malformed.push_back(reply);
    for (const auto& bytes : malformed) {
        WatchAncsSession engine;
        const auto session = Ready(engine);
        Ack(engine, StartApp(engine, session));
        assert(!Receive(engine, session, bytes));
        Failed(engine, WatchAncsError::kProtocol);
    }
    WatchAncsSession engine;
    const auto session = Ready(engine);
    auto request = StartApp(engine, session);
    Ack(engine, request);
    assert(engine.Clear(session));
    assert(Receive(engine, session, AppReply()));
    assert(engine.Snapshot().notifications.empty());
    // The cleared lookup must not populate the cache after the clear.
    request = StartApp(engine, session);
    assert(engine.Snapshot().notifications.front().app_name.empty());
    Ack(engine, request);
    assert(Receive(engine, session, AppReply("ai.audream.app", "")));
    const auto item = engine.Snapshot().notifications.front();
    assert(engine.RequestAppName(session, kUid, item.revision));
    assert(!engine.NextRequest(session, 102));
}

void TestRejectedRequestsDoNotClearUnrelatedNotifications() {
    WatchAncsSession engine;
    const auto session = Ready(engine);
    auto request = Start(engine, session, 1);
    Ack(engine, request);
    assert(Receive(engine, session, Reply(1)));
    request = Start(engine, session, 2);
    assert(engine.RequestWritten(session, request.request, kRejected, 102));
    assert(engine.Snapshot().state == WatchAncsState::kReady);
    assert(engine.Snapshot().notifications.size() == 1);
    assert(engine.Snapshot().notifications.front().uid == 1);
    request = Start(engine, session, 2);
    assert(Notify(engine, session, 2, 1));
    assert(engine.RequestWritten(session, request.request, kRejected, 102));
    auto retry = engine.NextRequest(session, 103);
    assert(retry && retry->uid == 2);
    assert(engine.RequestWritten(session, retry->request, kAccepted, 104));
    assert(Receive(engine, session, Reply(2), 105));
    const auto item = engine.Snapshot().notifications.front();
    assert(engine.RequestAppName(session, 2, item.revision));
    request = *engine.NextRequest(session, 106);
    assert(engine.RequestWritten(session, request.request, kRejected, 107));
    assert(engine.Snapshot().notifications.size() == 2);
    // A rejected write cannot legally have emitted response fragments.
    request = Start(engine, session, 3);
    const uint8_t prefix = 0;
    assert(engine.DataSource(session, &prefix, 1, 101));
    assert(!engine.RequestWritten(session, request.request, kRejected, 102));
    Failed(engine, WatchAncsError::kProtocol);
}
}

int main() {
    TestAuthorizationAndSessionRetirement();
    TestWireRequestAndEverySplit();
    TestInflightModificationRemovalAndClear();
    TestBoundedRetentionAndPendingEviction();
    TestGuardedLocalDismissal();
    TestFailuresAndTimeoutsRetirePrivateData();
    TestMalformedResponses();
    TestTextLimitsAndInvalidUtf8();
    TestAppNameWireFragmentsAndSessionCache();
    TestAppNameMalformedRepliesAndClearing();
    TestRejectedRequestsDoNotClearUnrelatedNotifications();
    std::puts("Eleven ANCS session test groups passed");
}
