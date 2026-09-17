#include "watch/watch_android_notifications.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Engine = WatchAndroidNotifications;
using State = WatchAndroidNotificationState;
using Result = Engine::Result;
using Bytes = std::vector<uint8_t>;
#define CHECK(value) do { if (!(value)) throw std::runtime_error("Check failed: " #value); } while (false)
void Put(Bytes& bytes, uint64_t value, size_t size) {
    for (size_t i = 0; i < size; ++i) bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
void Replace(Bytes& bytes, size_t offset, uint64_t value, size_t size) {
    for (size_t i = 0; i < size; ++i) bytes.at(offset + i) = static_cast<uint8_t>(value >> (8 * i));
}
void Text(Bytes& bytes, const std::string& value) {
    Put(bytes, value.size(), 2); bytes.insert(bytes.end(), value.begin(), value.end());
}
WatchAndroidNotification Item(uint32_t uid, const std::string& title = "Title") {
    WatchAndroidNotification item;
    item.uid = uid; item.posted_at_ms = 123456789; item.package_name = "ai.messages";
    item.app_name = "Messages"; item.title = title; item.message = "Line 1\nLine 2";
    return item;
}
Bytes Packet(uint64_t request, uint64_t revision, std::vector<WatchAndroidNotification> entries,
             uint8_t kind = 1, uint64_t session = 2) {
    Bytes bytes{'A','W','N','1',kind,0,0,0};
    Put(bytes, session, 8); Put(bytes, request, 8); Put(bytes, revision, 8); Put(bytes, 0, 4);
    if (kind != 3) {
        Put(bytes, entries.size(), 1);
        for (const auto& item : entries) {
            Put(bytes, item.uid, 4); Put(bytes, item.posted_at_ms, 8);
            Text(bytes, item.package_name); Text(bytes, item.app_name); Text(bytes, item.title); Text(bytes, item.message);
        }
    }
    Replace(bytes, 32, bytes.size() - Engine::kHeaderBytes, 4);
    return bytes;
}
uint64_t Ready(Engine& engine) {
    engine.SetEnabled(true);
    const auto token = engine.BeginAuthorizedSession(2, 100);
    CHECK(token != 0); return token;
}
Engine::ReceiveResult Feed(Engine& engine, uint64_t token, const Bytes& packet, size_t chunk = 20, uint64_t now = 100) {
    Engine::ReceiveResult result{Result::kIgnored, {}};
    for (size_t pos = 0; pos < packet.size(); pos += chunk) {
        const size_t size = std::min(chunk, packet.size() - pos);
        result = engine.Receive(token, packet.data() + pos, size, now);
        if (result.result == Result::kFailed || result.result == Result::kIgnored) return result;
        if (pos + size < packet.size()) {
            CHECK(result.result == Result::kReceiving); CHECK(result.acknowledgement.empty());
        }
    }
    return result;
}
void FragmentationAndAtomicApply() {
    const auto packet = Packet(3, 2, {Item(2, "Second"), Item(1, "\xe4\xbc\x9a\xe8\xae\xae\xf0\x9f\x98\x83")});
    for (size_t split = 1; split < packet.size(); ++split) {
        Engine engine; const auto token = Ready(engine);
        CHECK(engine.Receive(token, packet.data(), split, 100).result == Result::kReceiving);
        CHECK(engine.Snapshot(100).entries.empty());
        const auto result = engine.Receive(token, packet.data() + split, packet.size() - split, 101);
        CHECK(result.result == Result::kApplied); CHECK(result.acknowledgement.size() == 37);
        CHECK(result.acknowledgement[4] == 2 && result.acknowledgement[36] == 0);
        auto snapshot = engine.Snapshot(101);
        CHECK(snapshot.entries.size() == 2); CHECK(snapshot.entries[1].title == "\xe4\xbc\x9a\xe8\xae\xae\xf0\x9f\x98\x83");
        CHECK(snapshot.entries[1].posted_at_ms == 123456789);
        CHECK(snapshot.entries[0].uid == 2);
    }
    for (size_t chunk : {1U, 20U, 182U, 509U, 512U}) {
        Engine engine; auto token = Ready(engine); CHECK(Feed(engine, token, packet, chunk).result == Result::kApplied);
    }
}
void AuthorizationRevocationAndGenerations() {
    Engine engine;
    CHECK(engine.BeginAuthorizedSession(2, 100) == 0);
    auto bytes = Packet(3, 1, {Item(1)});
    CHECK(Feed(engine, 1, bytes).result == Result::kIgnored);
    const auto token = Ready(engine);
    CHECK(engine.BeginAuthorizedSession(2, 100) == 0);
    CHECK(Feed(engine, token, bytes).result == Result::kApplied);
    engine.SetEnabled(false);
    CHECK(engine.Snapshot(100).entries.empty());
    CHECK(Feed(engine, token, bytes).result == Result::kIgnored);
    const auto next = Ready(engine);
    CHECK(next != token);
    CHECK(engine.Receive(token, nullptr, 999999, 100).result == Result::kIgnored);
    engine.EndSession(token);
    CHECK(engine.Snapshot(100).session == next);
    CHECK(Feed(engine, next, bytes).result == Result::kApplied);
    auto wrong = Packet(4, 2, {Item(2)}, 1, 99);
    CHECK(Feed(engine, next, wrong).result == Result::kFailed);
    CHECK(engine.Snapshot(100).entries.empty());
    CHECK(engine.BeginAuthorizedSession(2, 100) == 0);
    engine.EndSession(next);
    CHECK(engine.BeginAuthorizedSession(2, 100) != 0);
}
void InvalidFramesPurgeAndQuarantine() {
    auto valid = Packet(3, 1, {Item(1)});
    std::vector<Bytes> invalid;
    for (auto offset : {0U, 4U, 5U, 6U, 7U, 8U}) {
        auto bytes = valid; bytes[offset] = 0xff; invalid.push_back(bytes);
    }
    for (auto value : {0ULL, 20542ULL, 0xffffffffULL}) {
        auto bytes = valid; Replace(bytes, 32, value, 4); invalid.push_back(bytes);
    }
    for (size_t offset : {16U, 24U}) {
        auto bytes = valid; Replace(bytes, offset, 0x8000000000000000ULL, 8); invalid.push_back(bytes);
    }
    auto count = valid; count[36] = 21; invalid.push_back(count);
    auto zero = valid; Replace(zero, 37, 0, 4); invalid.push_back(zero);
    auto length = valid; Replace(length, 49, 256, 2); invalid.push_back(length);
    auto trailing = valid; trailing.push_back(1); invalid.push_back(trailing);
    auto inner_trailing = valid; inner_trailing.push_back(1); Replace(inner_trailing, 32, inner_trailing.size() - 36, 4); invalid.push_back(inner_trailing);
    invalid.push_back(Packet(3, 1, {Item(1), Item(1)}));
    invalid.push_back(Packet(3, 1, {}, 3));
    const std::vector<std::string> bad_text{
        std::string("\x00", 1), "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80",
        "\xe2\x80\xae", "\xe2\x81\xa6", "\xc2\x80", "\xe4\xb8", "\x80", std::string(513, 'x')};
    for (const auto& text : bad_text) { auto item = Item(1); item.message = text; invalid.push_back(Packet(3, 1, {item})); }
    auto bad_package = Item(1); bad_package.package_name = "invalid-package"; invalid.push_back(Packet(3, 1, {bad_package}));
    auto bad_time = Item(1); bad_time.posted_at_ms = 0xffffffffffffffffULL; invalid.push_back(Packet(3, 1, {bad_time}));
    for (const auto& packet : invalid) {
        Engine engine; const auto token = Ready(engine);
        CHECK(Feed(engine, token, packet).result == Result::kFailed);
        CHECK(engine.Snapshot(100).state == State::kFailed);
        CHECK(engine.Snapshot(100).entries.empty());
        CHECK(Feed(engine, token, valid).result == Result::kIgnored);
    }
    Engine engine; auto token = Ready(engine);
    CHECK(Feed(engine, token, valid).result == Result::kApplied);
    CHECK(Feed(engine, token, valid).result == Result::kFailed);
    CHECK(engine.Snapshot(100).entries.empty());
}
void ClearsAndReadsDoNotResurrect() {
    Engine engine; auto token = Ready(engine);
    CHECK(Feed(engine, token, Packet(3, 2, {Item(2), Item(1)})).result == Result::kApplied);
    auto initial = engine.Snapshot(100);
    CHECK(engine.MarkRead(token, 1, initial.entries[1].revision, 100));
    CHECK(Feed(engine, token, Packet(4, 3, {Item(2, "Updated"), Item(1)})).result == Result::kApplied);
    auto changed = engine.Snapshot(100);
    CHECK(changed.entries[1].read); CHECK(changed.entries[1].revision == initial.entries[1].revision);
    CHECK(!engine.MarkRead(token, 2, initial.entries[0].revision, 100));
    CHECK(!engine.Dismiss(token, 1, changed.entries[1].revision + 1, 100));
    CHECK(engine.Dismiss(token, 1, changed.entries[1].revision, 100));
    CHECK(engine.Snapshot(100).entries.size() == 1 && engine.Snapshot(100).entries[0].uid == 2);
    CHECK(engine.Clear(token, 100));
    CHECK(Feed(engine, token, Packet(5, 4, {Item(3), Item(1)})).result == Result::kApplied);
    CHECK(engine.Snapshot(100).entries.size() == 1 && engine.Snapshot(100).entries[0].uid == 3);
    const auto pending = Packet(6, 5, {Item(4), Item(3)});
    CHECK(engine.Receive(token, pending.data(), 10, 100).result == Result::kReceiving);
    CHECK(engine.Clear(token, 100));
    CHECK(engine.Receive(token, pending.data() + 10, pending.size() - 10, 100).result == Result::kApplied);
    CHECK(engine.Snapshot(100).entries.empty());
    CHECK(Feed(engine, token, Packet(7, 6, {Item(5), Item(4)})).result == Result::kApplied);
    CHECK(engine.Snapshot(100).entries.size() == 1 && engine.Snapshot(100).entries[0].uid == 5);
    CHECK(Feed(engine, token, Packet(8, 7, {})).result == Result::kApplied);
    CHECK(engine.Snapshot(100).entries.empty());
}
void FixedDeadlinesAndHeartbeats() {
    for (uint64_t end : {99ULL, 5100ULL}) {
        Engine engine; auto token = Ready(engine); auto bytes = Packet(3, 1, {Item(1)});
        CHECK(engine.Receive(token, bytes.data(), 1, 100).result == Result::kReceiving);
        CHECK(engine.Receive(token, bytes.data() + 1, 1, 4099).result == Result::kReceiving);
        CHECK(engine.Poll(end) == State::kFailed);
        CHECK(engine.Snapshot(end).entries.empty());
    }
    Engine engine; auto token = Ready(engine);
    CHECK(Feed(engine, token, Packet(3, 1, {Item(1)})).result == Result::kApplied);
    CHECK(Feed(engine, token, Packet(4, 1, {}, 3), 20, 10100).result == Result::kApplied);
    CHECK(engine.Snapshot(30100).entries.size() == 1);
    CHECK(engine.Poll(40100) == State::kFailed);
    CHECK(engine.Snapshot(40100).entries.empty());
    engine.EndSession(token); token = engine.BeginAuthorizedSession(2, 100);
    CHECK(engine.Poll(30100) == State::kFailed);
    engine.EndSession(token); token = engine.BeginAuthorizedSession(2, 100);
    CHECK(Feed(engine, token, Packet(3, 1, {Item(1)})).result == Result::kApplied);
    CHECK(Feed(engine, token, Packet(4, 2, {}, 3)).result == Result::kFailed);
}
void MaximumBoundaries() {
    std::vector<WatchAndroidNotification> entries;
    for (uint32_t uid = 1; uid <= 20; ++uid) {
        auto item = Item(uid); item.package_name.assign(255, 'p'); item.app_name.assign(120, 'a');
        item.title.assign(120, 't'); item.message.assign(512, 'm'); entries.push_back(item);
    }
    auto bytes = Packet(3, 1, entries);
    CHECK(bytes.size() == Engine::kHeaderBytes + Engine::kMaxBodyBytes);
    Engine engine; const auto token = Ready(engine);
    CHECK(Feed(engine, token, bytes, 509).result == Result::kApplied);
    CHECK(engine.Snapshot(100).entries.size() == 20);
    CHECK(engine.Receive(token, bytes.data(), 513, 100).result == Result::kFailed);
}
void CrossLanguage(const std::string& directory) {
    Engine engine; const auto token = Ready(engine);
    for (const std::string name : {"initial", "replacement", "heartbeat", "empty"}) {
        std::ifstream input(directory + "/" + name + ".awn", std::ios::binary);
        CHECK(input.good());
        Bytes packet((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        CHECK(!packet.empty());
        const auto result = Feed(engine, token, packet, 7);
        CHECK(result.result == Result::kApplied);
        const auto snapshot = engine.Snapshot(100);
        if (name == "initial") {
            CHECK(snapshot.entries.size() == 2); CHECK(snapshot.entries[0].uid == 2);
            CHECK(snapshot.entries[1].title == "\xe4\xbc\x9a\xe8\xae\xae\xf0\x9f\x98\x83");
            CHECK(snapshot.entries[1].message == "Line 1\nLine 2");
            CHECK(snapshot.entries[1].posted_at_ms == 123456789);
        } else if (name == "empty") CHECK(snapshot.entries.empty());
        else { CHECK(snapshot.entries.size() == 2); CHECK(snapshot.entries[0].uid == 3); }
        std::ofstream output(directory + "/" + name + ".ack", std::ios::binary);
        output.write(reinterpret_cast<const char*>(result.acknowledgement.data()), result.acknowledgement.size());
        CHECK(output.good());
    }
}
}
int main(int argc, char** argv) {
    try {
        FragmentationAndAtomicApply(); AuthorizationRevocationAndGenerations(); InvalidFramesPurgeAndQuarantine();
        ClearsAndReadsDoNotResurrect(); FixedDeadlinesAndHeartbeats(); MaximumBoundaries();
        std::cout << "Android notification receiver: 6 groups passed\n";
        if (argc == 2) { CrossLanguage(argv[1]); std::cout << "Kotlin snapshot / C++ acknowledgement contract passed\n"; }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
