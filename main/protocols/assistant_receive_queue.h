#pragma once

#include "assistant_wire_format.h"

#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

// One queue per physical connection. Network callbacks only retain this queue,
// never the Protocol or Application that consumes it.
class AssistantReceiveQueue {
public:
    enum class Kind { Audio, Json, Disconnected, Overflow, ProtocolError };
    struct Event {
        Kind kind = Kind::Json;
        std::string payload;
        uint32_t timestamp = 0;
        int duration_ms = 0;
    };
    static constexpr size_t kMaxEvents = 512;
    static constexpr size_t kMaxBytes = 65536;
    static constexpr int kMaxAudioMs = 2400;

    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    explicit AssistantReceiveQueue(int version, bool allow_turns = false,
                                   Clock clock = std::chrono::steady_clock::now)
        : version_(version), allow_turns_(allow_turns), clock_(std::move(clock)) {}
    bool PrepareControl(std::string& text, bool& new_turn);
    bool CanSendAudio() const;
    bool SuspendTurn();
    bool Receive(const char* data, size_t size, bool binary);
    bool Disconnect();
    void Retire();
    bool Ready() const;
    bool TimedOut() const;
    std::optional<assistant_wire::ServerHello> Hello() const;
    std::optional<Event> Pop();
    bool HasEvents() const;

private:
    const int version_;
    const bool allow_turns_;
    const Clock clock_;
    mutable std::mutex mutex_;
    bool retired_ = false;
    bool connected_ = true;
    std::optional<assistant_wire::ServerHello> hello_;
    std::optional<Kind> terminal_;
    std::deque<Event> events_;
    size_t bytes_ = 0;
    int audio_ms_ = 0;
    int turn_id_ = 0;
    bool turn_accepted_ = false;
    bool turn_suspended_ = false;
    std::chrono::steady_clock::time_point turn_deadline_{};
    std::chrono::steady_clock::time_point last_received_{};

    void Terminate(Kind kind);
    bool TurnTimedOut() const;
};
