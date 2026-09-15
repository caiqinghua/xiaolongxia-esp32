#pragma once

#include "audio/audio_session_coordinator.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class WatchAssistantPhase { kReady, kSetupRequired, kOffline, kBusy, kConnecting,
    kListening, kThinking, kSpeaking, kStopping, kFailed, kDisabled };
enum class WatchAssistantError { kNone, kNetwork, kAudio, kTimeout, kProtocol };
enum class WatchAssistantRole { kUser, kAssistant };

struct WatchAssistantMessage {
    WatchAssistantRole role;
    std::string text;
};

struct WatchAssistantSnapshot {
    WatchAssistantPhase phase = WatchAssistantPhase::kSetupRequired;
    WatchAssistantError error = WatchAssistantError::kNone;
    uint64_t revision = 0;
    std::vector<WatchAssistantMessage> messages;
    bool can_talk = false;
    bool can_connect = false;
    bool can_send = false;
    bool can_interrupt = false;
    bool can_end = false;
};

// Owner-task presentation state; no hardware, persistence, or optimistic phases.
class WatchAssistantState {
public:
    static constexpr size_t kMaxMessages = 6;
    static constexpr size_t kMaxMessageBytes = 512;
    void BeginTurn();
    void Fail(WatchAssistantError error);
    void SetUserText(std::string_view text);
    void AppendAssistantText(std::string_view text);
    void Clear();
    WatchAssistantSnapshot Snapshot(AudioSessionMode mode, bool closing, bool pending,
        bool configured, bool connected, bool playing_tail, bool enabled = true,
        bool conversation_ready = false) const;

private:
    WatchAssistantError error_ = WatchAssistantError::kNone;
    uint64_t revision_ = 0;
    bool user_started_ = false;
    bool assistant_started_ = false;
    std::vector<WatchAssistantMessage> messages_;
    void Append(WatchAssistantRole role, std::string text);
};
