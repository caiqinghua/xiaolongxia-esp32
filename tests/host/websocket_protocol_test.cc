#include "protocols/websocket_protocol.h"
#include "protocols/assistant_wire_format.h"
#include "audio/assistant_session_lifecycle.h"
#include "board.h"
#include "settings.h"
#include <esp_timer.h>
#include <cassert>
#include <iostream>

static constexpr const char* kHello = R"({"type":"hello","transport":"websocket","session_id":"session-one","audio_params":{"sample_rate":16000,"frame_duration":20}})";

static void Reset() {
    assert(WebSocket::current == nullptr && host_event_groups == 0);
    WebSocket::hello_hook = [](auto& socket) { socket.Receive(kHello); };
    WebSocket::connect_ok = WebSocket::send_ok = true;
    WebSocket::disconnect_ready = true;
    host_wait_hook = {};
    Settings::version = 1;
    host_monotonic_ms = 1000;
    Board::GetInstance().available = true;
    Board::GetInstance().network.create_ok = true;
}

static void TestCloudAuthorizationAndExpiry() {
    Reset();
    watch_voice::Bootstrap authorization;
    authorization.settings.enabled = true;
    authorization.settings.activation_mode = "push_to_talk";
    authorization.settings.revision = 2;
    authorization.protocol_version = 3;
    authorization.websocket_url = "wss://voice.example/v1/device";
    authorization.access_token = "aaa.bbb.ccc";
    authorization.expires_at_ms = 61000;
    const std::string device = "11111111-2222-3333-4444-555555555555";
    WebsocketProtocol protocol;
    assert(protocol.SetCloudAuthorization(authorization, device));
    assert(protocol.OpenAudioChannel());
    assert(WebSocket::current->url == authorization.websocket_url);
    assert(WebSocket::current->headers["Authorization"] == "Bearer aaa.bbb.ccc");
    assert(WebSocket::current->headers["Device-Id"] == device);
    assert(WebSocket::current->headers["Protocol-Version"] == "3");
    assert(!protocol.SetCloudAuthorization(authorization, device));
    host_monotonic_ms = 56000;
    assert(!protocol.IsAudioChannelOpened());
    protocol.Poll();
    assert(!protocol.OpenAudioChannel() && WebSocket::current == nullptr);
    host_monotonic_ms = 1000;
    assert(!protocol.OpenAudioChannel());
    assert(protocol.SetCloudAuthorization(authorization, device) && protocol.OpenAudioChannel());
    protocol.ClearCloudAuthorization();
    assert(!protocol.OpenAudioChannel() && WebSocket::current == nullptr);
    for (const auto* invalid : {"http://voice.example/v1/device", "ws://voice.example/v1/device", "wss://voice.example/v1/device?token=secret"}) {
        authorization.websocket_url = invalid;
        assert(!protocol.SetCloudAuthorization(authorization, device) && !protocol.OpenAudioChannel());
    }
    authorization.websocket_url = "wss://voice.example/v1/device";
    authorization.settings.save_transcript = true;
    assert(!protocol.SetCloudAuthorization(authorization, device) && !protocol.OpenAudioChannel());
    authorization.settings.save_transcript = false;
    authorization.settings.activation_mode = "wake_word";
    authorization.settings.wake_word_enabled = true;
    authorization.settings.power_mode = "always_available";
    assert(protocol.SetCloudAuthorization(authorization, device) && protocol.OpenAudioChannel());
    protocol.ClearCloudAuthorization();
    authorization.settings.power_mode = "battery_saver";
    assert(!protocol.SetCloudAuthorization(authorization, device));
    authorization.settings.power_mode = "always_available";
    authorization.settings.wake_word_enabled = false;
    assert(!protocol.SetCloudAuthorization(authorization, device));
    authorization.settings.wake_word_enabled = true;
    authorization.access_token += "\r\nInjected: true";
    assert(!protocol.SetCloudAuthorization(authorization, device) && !protocol.OpenAudioChannel());
}

static void TestHandshakeAndAudio() {
    for (int version : {1, 2, 3}) {
        Reset();
        Settings::version = version;
        WebsocketProtocol protocol;
        int received = 0;
        int json = 0;
        int opened = 0;
        protocol.OnIncomingAudio([&](auto packet) {
            ++received;
            assert(packet->sample_rate == 16000 && packet->frame_duration == 20);
            assert(packet->payload == std::vector<uint8_t>({1, 2, 3}));
            assert(packet->timestamp == (version == 2 ? 123U : 0));
        });
        protocol.OnIncomingJson([&](const auto*) { ++json; });
        protocol.OnAudioChannelOpened([&] { ++opened; });
        WebSocket::hello_hook = [&](auto& socket) {
            assert(!protocol.IsAudioChannelOpened());
            assert(!protocol.SendAudio(std::make_unique<AudioStreamPacket>()));
            socket.Receive("early audio", true);
            socket.Receive(R"({"type":"tts"})");
            assert(received == 0 && json == 0);
            socket.Receive(kHello);
        };
        assert(protocol.OpenAudioChannel());
        assert(opened == 1 && protocol.IsAudioChannelOpened());
        std::string wire;
        const uint8_t bytes[] = {1, 2, 3};
        assert(assistant_wire::EncodeAudio(version, bytes, sizeof(bytes), 123, wire));
        WebSocket::current->Receive(wire, true);
        assert(received == 0);
        protocol.Poll();
        assert(received == 1);
        WebSocket::current->Receive(R"({"type":"tts"})");
        protocol.Poll();
        assert(json == 1);
        WebSocket::current->Receive("{\"type\":\"tts\"}trailing");
        protocol.Poll();
        assert(json == 1);
        WebSocket::current->Receive(R"({"type":"hello","transport":"websocket","audio_params":{"sample_rate":48000}})");
        assert(protocol.server_sample_rate() == 16000);
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->payload = {1, 2, 3};
        packet->timestamp = 123;
        assert(protocol.SendAudio(std::move(packet)));
        assert(WebSocket::current->sent.back() == wire);
        assert(!protocol.SendAudio(nullptr));
        protocol.CloseAudioChannel();
        assert(!protocol.IsAudioChannelOpened());
    }
}

static void TestRejectedHelloAndTimeout() {
    for (const char* response : {"", "{}", R"({"type":"hello"})", R"({"type":"hello","transport":1})",
                                R"({"type":"hello","transport":"websocket","audio_params":{"channels":2}})"}) {
        Reset();
        WebsocketProtocol protocol;
        int opened = 0;
        protocol.OnAudioChannelOpened([&] { ++opened; });
        WebSocket::hello_hook = [=](auto& socket) { socket.Receive(response); };
        assert(!protocol.OpenAudioChannel());
        assert(opened == 0 && !protocol.IsAudioChannelOpened());
        assert(WebSocket::current == nullptr);
    }
}

static void TestReconnectReset() {
    Reset();
    WebsocketProtocol protocol;
    assert(protocol.OpenAudioChannel());
    assert(protocol.session_id() == "session-one");
    // A previously successful handshake must not satisfy the next connection.
    WebSocket::hello_hook = {};
    assert(!protocol.OpenAudioChannel());
    assert(protocol.session_id().empty());
    WebSocket::hello_hook = [](auto& socket) { socket.Receive(R"({"type":"hello","transport":"websocket"})"); };
    assert(protocol.OpenAudioChannel());
    assert(protocol.server_sample_rate() == 24000 && protocol.server_frame_duration() == 60);
    assert(protocol.session_id().empty());
}

static void TestConnectionFailures() {
    Reset();
    WebsocketProtocol protocol;
    Settings::version = 4;
    assert(!protocol.OpenAudioChannel() && WebSocket::current == nullptr);
    Settings::version = 1;
    Board::GetInstance().available = false;
    assert(!protocol.OpenAudioChannel() && WebSocket::current == nullptr);
    Board::GetInstance().available = true;
    Board::GetInstance().network.create_ok = false;
    assert(!protocol.OpenAudioChannel() && WebSocket::current == nullptr);
    Board::GetInstance().network.create_ok = true;
    WebSocket::connect_ok = false;
    assert(!protocol.OpenAudioChannel() && WebSocket::current == nullptr);
    WebSocket::connect_ok = true;
    WebSocket::send_ok = false;
    assert(!protocol.OpenAudioChannel() && WebSocket::current == nullptr);
    WebSocket::send_ok = true;
    host_wait_hook = [] { WebSocket::current->Disconnect(); };
    assert(!protocol.OpenAudioChannel() && WebSocket::current == nullptr);
    host_wait_hook = {};
}

static void TestLateCallbacksAndGuards() {
    Reset();
    std::function<void(const char*, size_t, bool)> late_data;
    std::function<void()> late_disconnect;
    std::function<bool()> guard;
    int audio = 0;
    int closed = 0;
    {
        WebsocketProtocol protocol;
        protocol.OnIncomingAudio([&](auto) { ++audio; });
        protocol.OnAudioChannelClosed([&] { ++closed; });
        assert(protocol.OpenAudioChannel());
        guard = protocol.CaptureEventGuard();
        assert(guard());
        late_data = WebSocket::current->data_callback;
        late_disconnect = WebSocket::current->disconnect_callback;
        WebSocket::current->Receive("queued old audio", true);
        assert(protocol.OpenAudioChannel());
        assert(!guard());
        const auto new_guard = protocol.CaptureEventGuard();
        assert(new_guard());
        assert(closed == 1);
        late_data("late old audio", 14, true);
        late_data(kHello, std::char_traits<char>::length(kHello), false);
        late_disconnect();
        protocol.Poll();
        assert(protocol.IsAudioChannelOpened());
        assert(audio == 0 && closed == 1 && new_guard());
        guard = new_guard;
    }
    assert(!guard());
    late_data("after destruction", 17, true);
    late_disconnect();
    assert(audio == 0 && closed == 1);
    late_data = {};
    late_disconnect = {};
    assert(host_event_groups == 0);
}

static void TestDeferredOrderAndOverflow() {
    Reset();
    WebsocketProtocol protocol;
    std::vector<std::string> order;
    int wakeups = 0;
    int errors = 0;
    int closed = 0;
    protocol.OnReceiveReady([&] { ++wakeups; });
    protocol.OnNetworkError([&](const auto&) { ++errors; });
    protocol.OnAudioChannelClosed([&] { ++closed; });
    protocol.OnIncomingJson([&](const cJSON*) { order.push_back("state"); });
    protocol.OnIncomingAudio([&](auto) { order.push_back("audio"); });
    assert(protocol.OpenAudioChannel());
    WebSocket::current->Receive(R"({"type":"tts","state":"start"})");
    WebSocket::current->Receive("audio", true);
    assert(order.empty());
    protocol.Poll();
    assert(order == std::vector<std::string>{"state"});
    const int previous_wakeups = wakeups;
    protocol.Poll();
    assert(order == std::vector<std::string>({"state", "audio"}));
    assert(wakeups == previous_wakeups);
    // Hello negotiated 20 ms, so 120 packets fit and the next ends the session.
    for (int i = 0; i < 121; ++i) WebSocket::current->Receive("audio", true);
    assert(!protocol.IsAudioChannelOpened());
    protocol.Poll();
    assert(errors == 1 && closed == 1 && WebSocket::current == nullptr);
    protocol.Poll();
    assert(errors == 1 && closed == 1 && order.size() == 2);
}

static void TestPendingCloseBlocksReuse() {
    Reset();
#if CONFIG_AUDREAM_WATCH_UI
    WebsocketProtocol protocol;
    int closed = 0;
    int received = 0;
    protocol.OnAudioChannelClosed([&] { ++closed; });
    protocol.OnIncomingAudio([&](auto) { ++received; });
    assert(protocol.OpenAudioChannel());
    auto old_socket = WebSocket::current;
    const auto guard = protocol.CaptureEventGuard();
    const int destructions = WebSocket::destructions;
    WebSocket::disconnect_ready = false;
    old_socket->Receive("queued audio", true);
    assert(!protocol.CloseAudioChannelWithin(10));
    assert(protocol.HasPendingClose() && !protocol.IsAudioChannelOpened() && !guard());
    assert(WebSocket::current == old_socket && WebSocket::destructions == destructions);
    assert(closed == 0);
    assert(!protocol.OpenAudioChannel());
    assert(WebSocket::current == old_socket && WebSocket::destructions == destructions);
    old_socket->Receive("late audio", true);
    protocol.Poll();
    assert(protocol.HasPendingClose() && received == 0 && closed == 0);
    protocol.SendUserText("must not be sent");
    assert(old_socket->sent.size() == 1);
    WebSocket::disconnect_ready = true;
    protocol.Poll();
    assert(!protocol.HasPendingClose() && WebSocket::current == nullptr && closed == 1);
    assert(protocol.CloseAudioChannelWithin(0) && closed == 1);
    assert(protocol.OpenAudioChannel());
    assert(protocol.IsAudioChannelOpened());
#else
    WebsocketProtocol protocol;
    assert(protocol.OpenAudioChannel());
    assert(!protocol.CloseAudioChannelWithin(10));
    assert(protocol.IsAudioChannelOpened() && !protocol.HasPendingClose());
    protocol.CloseAudioChannel();
    assert(protocol.CloseAudioChannelWithin(0));
#endif
}

#if CONFIG_AUDREAM_WATCH_UI
static void TestLiveTurnLeaseHandoff() {
    Reset();
    WebSocket::hello_hook = [](auto& socket) {
        socket.Receive(R"({"type":"hello","transport":"websocket","session_id":"one","features":{"audream_turns":true}})");
    };
    WebsocketProtocol protocol;
    AudioSessionCoordinator coordinator;
    bool drained = true;
    int received = 0;
    int resumed = 0;
    AssistantSessionLifecycle session(coordinator,
        [&](uint32_t timeout) { return protocol.CloseAudioChannelWithin(timeout); },
        [&](uint32_t) { return drained; }, [&] { ++resumed; return true; });
    const auto first = session.Begin();
    assert(first && protocol.OpenAudioChannel());
    protocol.SendStartListening(kListeningModeManualStop);
    WebSocket::current->Receive(R"({"type":"turn","state":"accepted","session_id":"one","turn_id":1})");
    protocol.OnIncomingAudio([&](auto) { if (session.IsActive(session.Token())) ++received; });
    auto socket = WebSocket::current;
    const auto guard = protocol.CaptureEventGuard();
    socket->Receive("queued old audio", true);
    drained = false;
    assert(protocol.SuspendIncomingTurn() && session.BeginNextTurn(first));
    const auto second = session.Token();
    assert(!guard() && !session.IsActive(first) && !session.IsActive(second));
    assert(!session.PollNextTurn() && coordinator.BlocksSleep());
    socket->Receive("late old audio", true);
    protocol.Poll();
    assert(received == 0 && resumed == 1 && WebSocket::current == socket);
    drained = true;
    assert(session.PollNextTurn() && session.IsActive(second) && resumed == 2);
    protocol.SendStartListening(kListeningModeManualStop);
    assert(socket->sent.back().find("\"turn_id\":2") != std::string::npos);
    socket->Receive("old audio before new barrier", true);
    protocol.Poll();
    assert(received == 0);
    socket->Receive(R"({"type":"turn","state":"accepted","session_id":"one","turn_id":2})");
    socket->Receive("new audio", true);
    protocol.Poll();
    assert(received == 1 && WebSocket::current == socket);
    assert(!session.Stop(first) && session.Stop(second));
    assert(!coordinator.BlocksSleep() && WebSocket::current == nullptr);
}

static void TestNegotiatedTurns() {
    for (int version : {1, 2, 3}) {
        Reset();
        Settings::version = version;
        WebsocketProtocol protocol;
        WebSocket::hello_hook = [](auto& socket) {
            assert(socket.sent.back().find("\"audream_turns\":true") != std::string::npos);
            socket.Receive(R"({"type":"hello","transport":"websocket","session_id":"one","features":{"audream_turns":true}})");
        };
        assert(protocol.OpenAudioChannel());
        auto guard = protocol.CaptureEventGuard();
        int audio = 0;
        int json = 0;
        protocol.OnIncomingAudio([&](auto) { ++audio; });
        protocol.OnIncomingJson([&](auto) { ++json; });
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->payload = {1, 2, 3};
        assert(!protocol.SendAudio(std::move(packet)));
        protocol.SendStartListening(kListeningModeManualStop);
        assert(!guard());
        assert(WebSocket::current->sent.back().find("\"turn_id\":1") != std::string::npos);
        guard = protocol.CaptureEventGuard();
        std::string wire;
        const uint8_t bytes[] = {1, 2, 3};
        assert(assistant_wire::EncodeAudio(version, bytes, sizeof(bytes), 123, wire));
        packet = std::make_unique<AudioStreamPacket>();
        packet->payload = {1, 2, 3};
        assert(protocol.SendAudio(std::move(packet)));
        WebSocket::current->Receive(wire, true);
        protocol.Poll();
        assert(audio == 0);
        WebSocket::current->Receive(R"({"type":"turn","state":"accepted","session_id":"one","turn_id":1})");
        protocol.Poll();
        assert(json == 0);
        WebSocket::current->Receive(wire, true);
        WebSocket::current->Receive(R"({"type":"tts","state":"start","session_id":"one","turn_id":1})");
        assert(protocol.SupportsConversationTurns() && protocol.SuspendIncomingTurn());
        assert(!guard());
        protocol.Poll();
        assert(audio == 0 && json == 0);
        WebSocket::current->Receive(wire, true);
        WebSocket::current->Receive(R"({"type":"tts","state":"stop","session_id":"one","turn_id":1})");
        protocol.Poll();
        assert(audio == 0 && json == 0 && protocol.IsAudioChannelOpened());
        packet = std::make_unique<AudioStreamPacket>();
        packet->payload = {1, 2, 3};
        assert(!protocol.SendAudio(std::move(packet)));
        protocol.SendStartListening(kListeningModeManualStop);
        assert(!guard());
        protocol.Poll();
        assert(audio == 0 && json == 0);
        WebSocket::current->Receive(wire, true);
        WebSocket::current->Receive(R"({"type":"turn","state":"accepted","session_id":"one","turn_id":1})");
        protocol.Poll();
        assert(audio == 0 && json == 0);
        WebSocket::current->Receive(R"({"type":"turn","state":"accepted","session_id":"one","turn_id":2})");
        WebSocket::current->Receive(R"({"type":"tts","state":"stop","session_id":"one","turn_id":1})");
        WebSocket::current->Receive(R"({"type":"tts","state":"start","session_id":"one","turn_id":2})");
        WebSocket::current->Receive(wire, true);
        protocol.Poll();
        protocol.Poll();
        assert(audio == 1 && json == 1);
        protocol.SendStopListening();
        assert(WebSocket::current->sent.back().find("\"turn_id\":2") != std::string::npos);
        protocol.SendMcpMessage(R"({"id":2,"result":{}})");
        assert(WebSocket::current->sent.back().find("\"turn_id\":2") != std::string::npos);
        protocol.SendAbortSpeaking(kAbortReasonNone);
        assert(WebSocket::current->sent.back().find("\"turn_id\":2") != std::string::npos);
        assert(protocol.OpenAudioChannel());
        protocol.SendStartListening(kListeningModeManualStop);
        assert(WebSocket::current->sent.back().find("\"turn_id\":1") != std::string::npos);
    }
}

static void TestSessionLeaseAndTransportCleanup() {
    Reset();
    WebsocketProtocol protocol;
    AudioSessionCoordinator coordinator;
    bool drained = false;
    unsigned int resumes = 0;
    unsigned int received = 0;
    AssistantSessionLifecycle session(coordinator,
        [&](uint32_t timeout) { return protocol.CloseAudioChannelWithin(timeout); },
        [&](uint32_t) { return drained; }, [&]() { ++resumes; return true; });
    auto token = session.Begin();
    assert(token && protocol.OpenAudioChannel() && coordinator.BlocksSleep());
    protocol.OnIncomingAudio([&](auto) { if (session.IsActive(token)) ++received; });
    const auto old_token = token;
    const auto old_guard = protocol.CaptureEventGuard();
    auto late_data = WebSocket::current->data_callback;
    WebSocket::current->Receive("queued audio", true);
    WebSocket::disconnect_ready = false;
    assert(!session.Stop(token));
    assert(!old_guard() && !session.IsActive(token) && coordinator.BlocksSleep());
    assert(session.Begin() == 0 && protocol.HasPendingClose());
    late_data("late audio", 10, true);
    protocol.Poll();
    assert(received == 0 && resumes == 1);
    WebSocket::disconnect_ready = true;
    protocol.Poll();
    assert(!protocol.HasPendingClose() && WebSocket::current == nullptr);
    assert(!session.Poll() && coordinator.BlocksSleep());
    drained = true;
    assert(session.Poll() && !coordinator.BlocksSleep() && resumes == 2);
    token = session.Begin();
    assert(token != old_token && protocol.OpenAudioChannel());
    late_data("old audio", 9, true);
    protocol.Poll();
    assert(received == 0 && !session.Stop(old_token));
    WebSocket::current->Receive("new audio", true);
    protocol.Poll();
    assert(received == 1 && session.Stop(token));
    late_data = {};
}

static void TestFailedHandshakeRetainsSessionLease() {
    Reset();
    WebsocketProtocol protocol;
    AudioSessionCoordinator coordinator;
    unsigned int drains = 0;
    AssistantSessionLifecycle session(coordinator,
        [&](uint32_t timeout) { return protocol.CloseAudioChannelWithin(timeout); },
        [&](uint32_t) { ++drains; return true; }, []() { return true; });
    const auto token = session.Begin();
    WebSocket::hello_hook = {};
    WebSocket::disconnect_ready = false;
    assert(!protocol.OpenAudioChannel());
    assert(protocol.HasPendingClose() && !session.Stop(token));
    assert(drains == 0 && coordinator.BlocksSleep() && session.IsClosing());
    WebSocket::disconnect_ready = true;
    assert(session.Poll());
    assert(drains == 1 && !coordinator.BlocksSleep() && WebSocket::current == nullptr);
}

static void TestCloudExpiryRetainsSessionLeaseUntilTransportStops() {
    Reset();
    WebsocketProtocol protocol;
    AudioSessionCoordinator coordinator;
    bool drained = false;
    AssistantSessionLifecycle session(coordinator,
        [&](uint32_t timeout) { return protocol.CloseAudioChannelWithin(timeout); },
        [&](uint32_t) { return drained; }, [] { return true; });
    watch_voice::Bootstrap authorization;
    authorization.settings.enabled = true;
    authorization.settings.activation_mode = "push_to_talk";
    authorization.settings.revision = 1;
    authorization.protocol_version = 1;
    authorization.access_token = "aaa.bbb.ccc";
    authorization.websocket_url = "wss://voice.example/v1/device";
    authorization.expires_at_ms = 61000;
    const std::string device = "11111111-2222-3333-4444-555555555555";
    assert(protocol.SetCloudAuthorization(authorization, device));
    const auto token = session.Begin();
    assert(token && protocol.OpenAudioChannel());
    WebSocket::disconnect_ready = false;
    host_monotonic_ms = 61000;
    protocol.Poll();
    assert(protocol.HasPendingClose() && !session.Stop(token));
    assert(coordinator.BlocksSleep() && !protocol.SetCloudAuthorization(authorization, device));
    WebSocket::disconnect_ready = true;
    protocol.Poll();
    assert(!session.Poll() && coordinator.BlocksSleep());
    drained = true;
    assert(session.Poll() && !coordinator.BlocksSleep());
    assert(!protocol.OpenAudioChannel());
}
#endif

int main() {
    TestCloudAuthorizationAndExpiry();
    TestHandshakeAndAudio();
    TestRejectedHelloAndTimeout();
    TestReconnectReset();
    TestConnectionFailures();
    TestLateCallbacksAndGuards();
    TestDeferredOrderAndOverflow();
    TestPendingCloseBlocksReuse();
#if CONFIG_AUDREAM_WATCH_UI
    TestLiveTurnLeaseHandoff();
    TestNegotiatedTurns();
    TestSessionLeaseAndTransportCleanup();
    TestFailedHandshakeRetainsSessionLease();
    TestCloudExpiryRetainsSessionLeaseUntilTransportStops();
#endif
    Reset();
    std::cout << "PASS: " << (CONFIG_AUDREAM_WATCH_UI ? 13 : 8)
              << " WebSocket protocol integration test groups (watch="
              << CONFIG_AUDREAM_WATCH_UI << ")\n";
}
