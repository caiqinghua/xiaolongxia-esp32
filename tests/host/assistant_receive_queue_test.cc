#include "protocols/assistant_receive_queue.h"

#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

static bool Receive(AssistantReceiveQueue& queue, const std::string& text, bool binary = false) {
    return queue.Receive(text.data(), text.size(), binary);
}

static void Hello(AssistantReceiveQueue& queue) {
    assert(Receive(queue, R"({"type":"hello","transport":"websocket"})"));
    assert(queue.Ready());
}

static void TestOrderingAndAccounting() {
    AssistantReceiveQueue queue(1);
    assert(!Receive(queue, "audio", true));
    assert(!Receive(queue, R"({"type":"tts","state":"start"})"));
    Hello(queue);
    assert(Receive(queue, R"({"type":"tts","state":"start"})"));
    assert(Receive(queue, "audio", true));
    assert(Receive(queue, R"({"type":"tts","state":"stop"})"));
    assert(queue.Pop()->kind == AssistantReceiveQueue::Kind::Json);
    auto audio = queue.Pop();
    assert(audio && audio->kind == AssistantReceiveQueue::Kind::Audio);
    assert(audio->payload == "audio" && audio->duration_ms == 60);
    assert(queue.Pop()->kind == AssistantReceiveQueue::Kind::Json);
    assert(!queue.Pop() && !queue.HasEvents());
    // Popping an event returns both byte and audio-duration capacity.
    for (int i = 0; i < 1000; ++i) {
        assert(Receive(queue, "a", true));
        assert(queue.Pop()->kind == AssistantReceiveQueue::Kind::Audio);
    }
    assert(queue.Ready());
}

static void TestAudioOverflow() {
    AssistantReceiveQueue queue(1);
    Hello(queue);
    for (int i = 0; i < AssistantReceiveQueue::kMaxAudioMs / 60; ++i) assert(Receive(queue, "a", true));
    assert(queue.Ready());
    assert(Receive(queue, "a", true));
    assert(!queue.Ready());
    assert(queue.Pop()->kind == AssistantReceiveQueue::Kind::Overflow);
    assert(!queue.Pop());
    assert(!Receive(queue, "a", true));
    assert(!queue.Disconnect());
}

static void TestByteAndCountLimits() {
    for (bool byte_limit : {false, true}) {
        AssistantReceiveQueue queue(1);
        Hello(queue);
        const std::string message = byte_limit
            ? "{\"type\":\"llm\",\"text\":\"" + std::string(40000, 'a') + "\"}"
            : "{\"type\":\"tts\"}";
        const size_t count = byte_limit ? 1 : AssistantReceiveQueue::kMaxEvents;
        for (size_t i = 0; i < count; ++i) assert(Receive(queue, message));
        assert(queue.Ready());
        assert(Receive(queue, message));
        assert(!queue.Ready());
        assert(queue.Pop()->kind == AssistantReceiveQueue::Kind::Overflow);
    }
}

static void TestDisconnectAndRetire() {
    AssistantReceiveQueue queue(1);
    Hello(queue);
    assert(Receive(queue, "audio", true));
    assert(queue.Disconnect());
    assert(!queue.Ready() && !queue.Hello());
    assert(queue.Pop()->kind == AssistantReceiveQueue::Kind::Disconnected);
    assert(!queue.Pop() && !queue.Disconnect());
    queue.Retire();
    assert(!Receive(queue, R"({"type":"hello","transport":"websocket"})"));
    assert(!Receive(queue, "late audio", true));
    assert(!queue.HasEvents() && !queue.Hello());
}

static void TestConcurrentRetirement() {
    AssistantReceiveQueue queue(1);
    Hello(queue);
    std::atomic<bool> started{false};
    std::thread producer([&] {
        started = true;
        for (int i = 0; i < 5000; ++i) Receive(queue, "a", true);
    });
    while (!started.load()) std::this_thread::yield();
    queue.Retire();
    producer.join();
    assert(!queue.Ready() && !queue.HasEvents() && !queue.Pop());
}

static constexpr const char* kTurnHello = R"({"type":"hello","transport":"websocket","session_id":"one","features":{"audream_turns":true}})";

static std::string Control(const char* type, int turn, const char* state = "start") {
    return std::string("{\"session_id\":\"one\",\"type\":\"") + type + "\",\"state\":\"" + state +
        "\",\"turn_id\":" + std::to_string(turn) + "}";
}

static void TestTurnBarriers() {
    AssistantReceiveQueue queue(1, true);
    assert(Receive(queue, kTurnHello));
    assert(!queue.CanSendAudio());
    bool started = false;
    auto start = Control("listen", 999);
    assert(queue.PrepareControl(start, started) && started);
    assert(start.find("\"turn_id\":1") != std::string::npos);
    assert(queue.CanSendAudio());
    assert(!Receive(queue, "old audio before acceptance", true));
    assert(!Receive(queue, Control("tts", 1)));
    assert(!Receive(queue, R"({"type":"turn","state":"accepted","turn_id":true,"session_id":"one"})"));
    assert(!Receive(queue, R"({"type":"turn","state":"accepted","turn_id":1.5,"session_id":"one"})"));
    assert(!Receive(queue, R"({"type":"turn","state":"accepted","turn_id":1,"session_id":"other"})"));
    assert(Receive(queue, Control("turn", 1, "accepted")));
    assert(!queue.HasEvents());
    assert(Receive(queue, Control("tts", 1)));
    assert(Receive(queue, "old queued audio", true));
    start = Control("listen", 99);
    assert(queue.PrepareControl(start, started) && started);
    assert(start.find("\"turn_id\":2") != std::string::npos);
    assert(!queue.HasEvents());
    assert(!Receive(queue, Control("tts", 1)));
    assert(!Receive(queue, Control("turn", 1, "accepted")));
    assert(!Receive(queue, "old audio still in flight", true));
    auto stop = Control("listen", 1, "stop");
    assert(queue.PrepareControl(stop, started) && !started);
    assert(stop.find("\"turn_id\":2") != std::string::npos);
    assert(Receive(queue, Control("turn", 2, "accepted")));
    assert(!Receive(queue, Control("turn", 2, "accepted")));
    assert(!Receive(queue, Control("tts", 1)));
    assert(Receive(queue, Control("tts", 2)));
    assert(Receive(queue, "new audio", true));
    assert(queue.Pop()->kind == AssistantReceiveQueue::Kind::Json);
    assert(queue.Pop()->payload == "new audio");
    assert(!queue.Pop());
    assert(queue.SuspendTurn());
    auto tool = Control("mcp", 2);
    assert(!queue.PrepareControl(tool, started));
    assert(!queue.CanSendAudio());
    start = Control("listen", 3);
    assert(queue.PrepareControl(start, started) && started && queue.CanSendAudio());
}

static void TestTurnDeadlineAndNegotiation() {
    using namespace std::chrono;
    auto now = steady_clock::time_point{};
    AssistantReceiveQueue queue(1, true, [&] { return now; });
    assert(Receive(queue, kTurnHello));
    auto start = Control("listen", 1);
    bool started = false;
    assert(queue.PrepareControl(start, started));
    now += seconds(9);
    assert(!queue.TimedOut() && queue.CanSendAudio());
    now += seconds(1);
    assert(queue.TimedOut() && !queue.CanSendAudio());
    assert(!queue.PrepareControl(start, started));
    assert(queue.Pop()->kind == AssistantReceiveQueue::Kind::ProtocolError);
    assert(!queue.Ready() && !queue.Pop());
    assert(!Receive(queue, Control("turn", 1, "accepted")));

    AssistantReceiveQueue legacy(1);
    assert(!Receive(legacy, kTurnHello));
    Hello(legacy);
    auto text = Control("listen", 999);
    const auto original = text;
    assert(legacy.PrepareControl(text, started) && !started && text == original);
    assert(legacy.CanSendAudio());
    AssistantReceiveQueue fallback(1, true);
    Hello(fallback);
    assert(fallback.PrepareControl(text, started) && !started && text == original);
    assert(Receive(fallback, "legacy audio", true));
}

int main() {
    TestOrderingAndAccounting();
    TestAudioOverflow();
    TestByteAndCountLimits();
    TestDisconnectAndRetire();
    TestConcurrentRetirement();
    TestTurnBarriers();
    TestTurnDeadlineAndNegotiation();
    std::cout << "PASS: 7 assistant receive-queue test groups\n";
}
