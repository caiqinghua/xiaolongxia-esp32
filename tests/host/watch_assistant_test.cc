#include "watch/watch_assistant_state.h"
#include <cassert>
#include <iostream>

void TestAvailability() {
    WatchAssistantState assistant;
    auto state = assistant.Snapshot(AudioSessionMode::kIdle, false, false, false, false, false);
    assert(state.phase == WatchAssistantPhase::kSetupRequired && !state.can_talk);
    state = assistant.Snapshot(AudioSessionMode::kIdle, false, false, true, false, false);
    assert(state.phase == WatchAssistantPhase::kOffline && !state.can_talk);
    state = assistant.Snapshot(AudioSessionMode::kIdle, false, false, true, true, false);
    assert(state.phase == WatchAssistantPhase::kReady && state.can_talk && !state.can_end);
    assistant.Fail(WatchAssistantError::kAudio);
    state = assistant.Snapshot(AudioSessionMode::kIdle, false, false, true, true, false);
    assert(state.phase == WatchAssistantPhase::kFailed && state.error == WatchAssistantError::kAudio && state.can_talk);
    assistant.BeginTurn();
    assert(assistant.Snapshot(AudioSessionMode::kIdle, false, false, true, true, false).error == WatchAssistantError::kNone);
}

void TestSessionPhases() {
    WatchAssistantState assistant;
    AudioSessionCoordinator coordinator;
    auto lease = coordinator.Begin(AudioSessionMode::kAssistantConnecting);
    assert(coordinator.Complete(lease));
    auto state = assistant.Snapshot(coordinator.Mode(), false, false, true, true, false);
    assert(state.phase == WatchAssistantPhase::kConnecting && state.can_end && !state.can_talk);
    assert(coordinator.UpdateAssistant(AudioSessionMode::kAssistantListening, lease.generation));
    state = assistant.Snapshot(coordinator.Mode(), false, false, true, true, false);
    assert(state.can_send && state.can_end && !state.can_interrupt);
    assert(coordinator.UpdateAssistant(AudioSessionMode::kAssistantThinking, lease.generation));
    state = assistant.Snapshot(coordinator.Mode(), false, false, true, true, false);
    assert(state.phase == WatchAssistantPhase::kThinking && state.can_interrupt && !state.can_send);
    state = assistant.Snapshot(coordinator.Mode(), false, false, true, true, true);
    assert(state.phase == WatchAssistantPhase::kSpeaking && state.can_interrupt);
    auto close = coordinator.BeginAssistantCleanup(lease.generation);
    assert(close);
    state = assistant.Snapshot(coordinator.Mode(), true, false, true, true, false);
    assert(state.phase == WatchAssistantPhase::kStopping && !state.can_talk && !state.can_end);
    state = assistant.Snapshot(coordinator.Mode(), true, true, true, true, false);
    assert(state.can_end && !state.can_interrupt);
}

void TestPreemption() {
    WatchAssistantState assistant;
    for (auto mode : {AudioSessionMode::kLocalRecording, AudioSessionMode::kRecordingPaused,
        AudioSessionMode::kFileTransfer, AudioSessionMode::kMaintenance, AudioSessionMode::kSleeping, AudioSessionMode::kFault,
        AudioSessionMode::kLocalPlayback, AudioSessionMode::kPlaybackPaused}) {
        const auto state = assistant.Snapshot(mode, true, true, true, true, true);
        assert(state.phase == WatchAssistantPhase::kBusy);
        assert(!state.can_talk && !state.can_send && !state.can_interrupt);
    }
}

void TestTranscriptBounds() {
    WatchAssistantState assistant;
    assistant.SetUserText("first partial");
    assistant.SetUserText("final question");
    assistant.AppendAssistantText("first sentence");
    assistant.AppendAssistantText("second sentence");
    auto state = assistant.Snapshot(AudioSessionMode::kIdle, false, false, true, true, false);
    assert(state.messages.size() == 2);
    assert(state.messages[0].text == "final question");
    assert(state.messages[1].text == "first sentence\nsecond sentence");
    const auto revision = state.revision;
    for (int turn = 0; turn < 100; ++turn) {
        assistant.BeginTurn();
        assistant.SetUserText("question " + std::to_string(turn));
        assistant.AppendAssistantText(std::string(4096, 'a'));
    }
    state = assistant.Snapshot(AudioSessionMode::kIdle, false, false, true, true, false);
    assert(state.messages.size() == WatchAssistantState::kMaxMessages && state.revision > revision);
    size_t bytes = 0;
    for (const auto& message : state.messages) {
        assert(message.text.size() <= WatchAssistantState::kMaxMessageBytes);
        bytes += message.text.size();
    }
    assert(bytes <= WatchAssistantState::kMaxMessages * WatchAssistantState::kMaxMessageBytes);
    assert(state.messages[state.messages.size() - 2].text == "question 99");
    assistant.Clear();
    assert(assistant.Snapshot(AudioSessionMode::kIdle, false, false, true, true, false).messages.empty());
}

void TestTextBoundaries() {
    WatchAssistantState assistant;
    const std::string input = std::string(510, 'a') + "\xE8\xBF\xBD";
    assistant.SetUserText(input);
    auto state = assistant.Snapshot(AudioSessionMode::kIdle, false, false, true, true, false);
    assert(state.messages[0].text == std::string(510, 'a'));
    assistant.Clear();
    assistant.SetUserText("\xE8\xBF\xBD\xE9\x9F\xB3\nhello\tworld\r\x01");
    state = assistant.Snapshot(AudioSessionMode::kIdle, false, false, true, true, false);
    assert(state.messages[0].text == "\xE8\xBF\xBD\xE9\x9F\xB3\nhello world");
    assistant.Clear();
    assistant.SetUserText("\xC0\xAF\xED\xA0\x80\xF4\x90\x80\x80\x80");
    assert(assistant.Snapshot(AudioSessionMode::kIdle, false, false, true, true, false).messages.empty());
}

void TestDisabledPermission() {
    WatchAssistantState assistant;
    for (const auto mode : {AudioSessionMode::kIdle, AudioSessionMode::kAssistantConnecting,
        AudioSessionMode::kAssistantListening, AudioSessionMode::kAssistantSpeaking, AudioSessionMode::kAssistantThinking,
        AudioSessionMode::kLocalRecording}) {
        const auto state = assistant.Snapshot(mode, false, false, true, true, false, false);
        assert(!state.can_talk && !state.can_send && !state.can_interrupt);
        assert(state.phase == (AudioSessionCoordinator::IsAssistant(mode) ? WatchAssistantPhase::kStopping : WatchAssistantPhase::kDisabled));
    }
    const auto closing = assistant.Snapshot(AudioSessionMode::kTransitioning, true, false, true, true, false, false);
    assert(closing.phase == WatchAssistantPhase::kStopping);
    const auto reenabled = assistant.Snapshot(AudioSessionMode::kIdle, false, false, true, true, false, true);
    assert(reenabled.can_talk);
}

void TestConnectedConversationReady() {
    WatchAssistantState assistant;
    const auto state = assistant.Snapshot(AudioSessionMode::kAssistantThinking,
        false, false, true, true, false, true, true);
    assert(state.phase == WatchAssistantPhase::kReady && state.can_talk && state.can_end);
    assert(!state.can_send && !state.can_interrupt);
    const auto tail = assistant.Snapshot(AudioSessionMode::kAssistantThinking,
        false, false, true, true, true, true, true);
    assert(tail.phase == WatchAssistantPhase::kSpeaking && tail.can_interrupt);
    const auto preempted = assistant.Snapshot(AudioSessionMode::kLocalRecording,
        false, false, true, true, false, true, true);
    assert(preempted.phase == WatchAssistantPhase::kBusy && !preempted.can_talk && !preempted.can_end);
}

int main() {
    TestAvailability(); TestSessionPhases(); TestPreemption(); TestTranscriptBounds(); TestTextBoundaries();
    TestDisabledPermission();
    TestConnectedConversationReady();
    std::cout << "7 watch assistant state tests passed\n";
}
