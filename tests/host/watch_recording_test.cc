#include "watch/watch_recording_state.h"
#include <cassert>
#include <iostream>

int main() {
    using Command = WatchRecordingCommand;
    WatchRecordingSnapshot state;
    assert(!state.Can(Command::kStart) && !state.Can(Command::kStop));
    state.storage_mounted = true;
    assert(state.Can(Command::kStart) && !state.Can(Command::kHighlight));
    state.resources_blocked = true;
    assert(!state.Can(Command::kStart));
    state.resources_blocked = false;
    state.control.starting = true;
    assert(!state.Can(Command::kStart) && state.Can(Command::kStop));
    assert(!state.Can(Command::kPause) && !state.Can(Command::kResume));
    state.control.starting = false;
    state.active = true;
    assert(state.Can(Command::kStop) && state.Can(Command::kPause) && state.Can(Command::kHighlight));
    assert(!state.Can(Command::kStart) && !state.Can(Command::kResume));
    state.paused = true;
    assert(state.Can(Command::kResume) && !state.Can(Command::kHighlight) && !state.Can(Command::kPause));
    state.transitioning = state.stopping = true;
    state.control.error = WatchRecordingError::kSaveFailed;
    assert(state.Can(Command::kStop) && !state.Can(Command::kResume));
    state.resources_blocked = true;
    assert(state.Can(Command::kStop) && !state.Can(Command::kStart));
    std::cout << "PASS: recording availability, pending cancellation, capture, pause, and failed drain controls\n";
}
