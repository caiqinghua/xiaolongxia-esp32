#include "watch_assistant_state.h"
#include <utility>

namespace {
std::string DisplayText(std::string_view source) {
    std::string result;
    result.reserve(WatchAssistantState::kMaxMessageBytes);
    for (size_t i = 0; i < source.size() && result.size() < WatchAssistantState::kMaxMessageBytes;) {
        const auto first = static_cast<uint8_t>(source[i]);
        if (first < 0x80) {
            if (first >= 0x20 || first == '\n') result.push_back(static_cast<char>(first));
            else if (first == '\t') result.push_back(' ');
            ++i;
            continue;
        }
        const size_t length = first >= 0xC2 && first <= 0xDF ? 2 :
            first >= 0xE0 && first <= 0xEF ? 3 : first >= 0xF0 && first <= 0xF4 ? 4 : 0;
        bool valid = length && length <= source.size() - i;
        for (size_t j = 1; valid && j < length; ++j) {
            const auto c = static_cast<uint8_t>(source[i + j]);
            valid = c >= 0x80 && c <= 0xBF;
        }
        if (valid && length >= 3) {
            const auto second = static_cast<uint8_t>(source[i + 1]);
            valid = !(first == 0xE0 && second < 0xA0) && !(first == 0xED && second >= 0xA0) &&
                !(first == 0xF0 && second < 0x90) && !(first == 0xF4 && second >= 0x90);
        }
        if (!valid) { ++i; continue; }
        if (length > WatchAssistantState::kMaxMessageBytes - result.size()) break;
        result.append(source.substr(i, length));
        i += length;
    }
    return result;
}
}

void WatchAssistantState::BeginTurn() {
    error_ = WatchAssistantError::kNone;
    user_started_ = assistant_started_ = false;
}

void WatchAssistantState::Fail(WatchAssistantError error) { error_ = error; }

void WatchAssistantState::Append(WatchAssistantRole role, std::string text) {
    if (messages_.size() == kMaxMessages) messages_.erase(messages_.begin());
    messages_.push_back({role, std::move(text)});
    ++revision_;
}

void WatchAssistantState::SetUserText(std::string_view text) {
    auto bounded = DisplayText(text);
    if (bounded.empty()) return;
    if (user_started_ && !assistant_started_ && !messages_.empty() && messages_.back().role == WatchAssistantRole::kUser) {
        if (messages_.back().text != bounded) { messages_.back().text = std::move(bounded); ++revision_; }
    } else {
        Append(WatchAssistantRole::kUser, std::move(bounded));
    }
    user_started_ = true;
}

void WatchAssistantState::AppendAssistantText(std::string_view text) {
    auto bounded = DisplayText(text);
    if (bounded.empty()) return;
    if (assistant_started_ && !messages_.empty() && messages_.back().role == WatchAssistantRole::kAssistant &&
        messages_.back().text.size() + bounded.size() + 1 <= kMaxMessageBytes) {
        messages_.back().text += '\n';
        messages_.back().text += bounded;
        ++revision_;
    } else {
        Append(WatchAssistantRole::kAssistant, std::move(bounded));
    }
    assistant_started_ = true;
}

void WatchAssistantState::Clear() {
    messages_.clear();
    BeginTurn();
    ++revision_;
}

WatchAssistantSnapshot WatchAssistantState::Snapshot(AudioSessionMode mode, bool closing, bool pending,
    bool configured, bool connected, bool playing_tail, bool enabled, bool conversation_ready) const {
    conversation_ready = conversation_ready && mode == AudioSessionMode::kAssistantThinking;
    WatchAssistantSnapshot state;
    state.messages = messages_;
    state.revision = revision_;
    state.error = error_;
    if (!enabled) state.phase = closing || pending || AudioSessionCoordinator::IsAssistant(mode) ?
        WatchAssistantPhase::kStopping : WatchAssistantPhase::kDisabled;
    else if (mode != AudioSessionMode::kIdle && mode != AudioSessionMode::kTransitioning &&
        !AudioSessionCoordinator::IsAssistant(mode)) state.phase = WatchAssistantPhase::kBusy;
    else if (closing) state.phase = WatchAssistantPhase::kStopping;
    else if (mode != AudioSessionMode::kIdle && !AudioSessionCoordinator::IsAssistant(mode)) state.phase = WatchAssistantPhase::kBusy;
    else if (mode == AudioSessionMode::kAssistantConnecting || pending) state.phase = WatchAssistantPhase::kConnecting;
    else if (mode == AudioSessionMode::kAssistantListening) state.phase = WatchAssistantPhase::kListening;
    else if (mode == AudioSessionMode::kAssistantSpeaking || playing_tail) state.phase = WatchAssistantPhase::kSpeaking;
    else if (mode == AudioSessionMode::kAssistantThinking && !conversation_ready) state.phase = WatchAssistantPhase::kThinking;
    else if (!configured) state.phase = WatchAssistantPhase::kSetupRequired;
    else if (!connected) state.phase = WatchAssistantPhase::kOffline;
    else if (error_ != WatchAssistantError::kNone) state.phase = WatchAssistantPhase::kFailed;
    else state.phase = WatchAssistantPhase::kReady;
    state.can_talk = configured && connected && (state.phase == WatchAssistantPhase::kReady || state.phase == WatchAssistantPhase::kFailed);
    state.can_send = state.phase == WatchAssistantPhase::kListening;
    state.can_interrupt = configured && connected &&
        (state.phase == WatchAssistantPhase::kThinking || state.phase == WatchAssistantPhase::kSpeaking);
    state.can_end = conversation_ready || state.phase == WatchAssistantPhase::kConnecting || state.can_send ||
        state.phase == WatchAssistantPhase::kThinking || state.phase == WatchAssistantPhase::kSpeaking || (closing && pending) ||
        (!enabled && AudioSessionCoordinator::IsAssistant(mode));
    return state;
}
